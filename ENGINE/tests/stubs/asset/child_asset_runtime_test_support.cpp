#include "child_asset_runtime_test_support.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(ENGINE_WORLD_TESTS)

namespace {

using test_child_asset_runtime::AnchorSpec;

struct AssetsState {
    std::vector<std::unique_ptr<Asset>> owned_assets;
    bool active_assets_dirty = false;
    std::unordered_map<std::string, int> spawn_failures_remaining;
};

std::unordered_map<Assets*, AssetsState> g_assets_states;
std::unordered_map<const Asset*, std::vector<AnchorSpec>> g_anchor_specs;

AssetsState& require_assets_state(Assets* assets) {
    return g_assets_states.at(assets);
}

const AssetsState& require_assets_state(const Assets* assets) {
    return g_assets_states.at(const_cast<Assets*>(assets));
}

const AnchorSpec* find_anchor_spec(const Asset* asset, const std::string& name) {
    const auto asset_it = g_anchor_specs.find(asset);
    if (asset_it == g_anchor_specs.end()) {
        return nullptr;
    }
    const auto& anchors = asset_it->second;
    const auto it = std::find_if(anchors.begin(), anchors.end(), [&](const AnchorSpec& spec) {
        return spec.name == name;
    });
    return (it != anchors.end()) ? &(*it) : nullptr;
}

} // namespace

namespace test_child_asset_runtime {

Assets* create_assets_stub() {
    void* memory = ::operator new(sizeof(Assets), std::align_val_t(alignof(Assets)));
    auto* assets = reinterpret_cast<Assets*>(memory);
    g_assets_states.emplace(assets, AssetsState{});
    return assets;
}

void destroy_assets_stub(Assets* assets) {
    if (!assets) {
        return;
    }
    g_assets_states.erase(assets);
    ::operator delete(assets, std::align_val_t(alignof(Assets)));
}

std::unique_ptr<Asset> make_test_asset(const std::string& name,
                                       int world_x,
                                       int world_y,
                                       int world_z,
                                       int resolution_layer) {
    auto info = std::make_shared<AssetInfo>(name);
    Area spawn_area("child_asset_test_area", 0);
    auto asset = std::make_unique<Asset>(info,
                                         spawn_area,
                                         SDL_Point{world_x, world_z},
                                         0,
                                         std::string{},
                                         std::string{},
                                         resolution_layer);
    asset->move_to_world_position(world_x, world_y, world_z, resolution_layer);
    return asset;
}

Asset* attach_owned_asset(Assets* assets, std::unique_ptr<Asset> asset) {
    AssetsState& state = require_assets_state(assets);
    asset->set_assets(assets);
    asset->active = true;
    Asset* raw = asset.get();
    state.owned_assets.push_back(std::move(asset));
    return raw;
}

void set_spawn_failures(Assets& assets, const std::string& name, int failures) {
    AssetsState& state = require_assets_state(&assets);
    if (failures <= 0) {
        state.spawn_failures_remaining.erase(name);
        return;
    }
    state.spawn_failures_remaining[name] = failures;
}

void set_anchor(Asset& asset, const AnchorSpec& spec) {
    auto& anchors = g_anchor_specs[&asset];
    auto it = std::find_if(anchors.begin(), anchors.end(), [&](const AnchorSpec& candidate) {
        return candidate.name == spec.name;
    });
    if (it != anchors.end()) {
        *it = spec;
        return;
    }
    anchors.push_back(spec);
}

void clear_anchors(Asset& asset) {
    g_anchor_specs.erase(&asset);
}

std::size_t asset_count(const Assets& assets) {
    return require_assets_state(&assets).owned_assets.size();
}

bool active_assets_dirty(const Assets& assets) {
    return require_assets_state(&assets).active_assets_dirty;
}

void clear_active_assets_dirty(Assets& assets) {
    require_assets_state(&assets).active_assets_dirty = false;
}

} // namespace test_child_asset_runtime

void Asset::Delete() {
    if (dead) {
        return;
    }
    dead = true;
    active = false;
    if (assets_) {
        assets_->schedule_removal(this);
    }
}

void Asset::move_to_world_position(int world_x,
                                   int world_y,
                                   int world_z,
                                   std::optional<int> resolution_layer_override) {
    const int resolved_layer = resolution_layer_override.has_value()
        ? vibble::grid::clamp_resolution(*resolution_layer_override)
        : grid_resolution;
    set_provisional_grid_point(world_x, world_y, world_z, resolved_layer);
    grid_resolution = resolved_layer;
}

void Asset::set_render_depth_bias(double bias) {
    render_depth_bias_ = bias;
}

void Asset::add_child(Asset* child) {
    if (!child || child == this) {
        return;
    }
    if (std::find(children_.begin(), children_.end(), child) != children_.end()) {
        return;
    }
    children_.push_back(child);
}

void Asset::remove_child(Asset* child) {
    if (!child) {
        return;
    }
    auto it = std::remove(children_.begin(), children_.end(), child);
    children_.erase(it, children_.end());
}

bool Asset::has_child(const Asset* child) const {
    return child && std::find(children_.begin(), children_.end(), child) != children_.end();
}

void Asset::set_hidden(bool state) {
    hidden = state;
}

bool Asset::is_hidden() const {
    return hidden;
}

void Asset::set_anchor_hidden(bool state) {
    anchor_hidden_ = state;
}

bool Asset::is_anchor_hidden() const {
    return anchor_hidden_;
}

std::optional<std::string> Asset::anchor_name_for_index(std::size_t index) const {
    const auto it = g_anchor_specs.find(this);
    if (it == g_anchor_specs.end() || index >= it->second.size()) {
        return std::nullopt;
    }
    return it->second[index].name;
}

std::optional<AnchorPoint> Asset::anchor_state(const std::string& name,
                                               anchor_points::GridMaterialization,
                                               AnchorResolveMode) {
    const AnchorSpec* spec = find_anchor_spec(this, name);
    if (!spec) {
        return std::nullopt;
    }

    AnchorPoint anchor{};
    anchor.name = spec->name;
    anchor.exists = spec->exists;
    anchor.depth_offset = spec->depth_offset;
    const float exact_world_x = spec->exact_offset_x.has_value()
        ? static_cast<float>(world_x()) + *spec->exact_offset_x
        : static_cast<float>(world_x() + spec->offset_x);
    const float exact_world_y = spec->exact_offset_y.has_value()
        ? static_cast<float>(world_y()) + *spec->exact_offset_y
        : static_cast<float>(world_y() + spec->offset_y);
    anchor.world_pos_2d = Vec2(exact_world_x, exact_world_y);
    anchor.world_exact_pos_2d = anchor.world_pos_2d;
    anchor.world_quantized_px = SDL_Point{
        static_cast<int>(std::lround(anchor.world_pos_2d.x)),
        static_cast<int>(std::lround(anchor.world_pos_2d.y))};
    anchor.world_z = world_z() + spec->offset_z;
    const float exact_world_z = spec->exact_offset_z.has_value()
        ? static_cast<float>(world_z()) + *spec->exact_offset_z
        : static_cast<float>(anchor.world_z) + spec->world_depth_offset;
    anchor.world_exact_z = exact_world_z;
    anchor.world_depth = exact_world_z;
    anchor.resolution_layer = spec->resolution_layer.value_or(grid_resolution);
    anchor.flip_horizontal = spec->flip_horizontal;
    anchor.flip_vertical = spec->flip_vertical;
    anchor.rotation_degrees = spec->rotation_degrees;
    anchor.hidden = spec->hidden;
    anchor.resolve_x = spec->resolve_x;
    return anchor;
}

bool Asset::set_anchor_sprite_transform_override(SDL_FlipMode flip, double angle_degrees) {
    if (!std::isfinite(angle_degrees)) {
        angle_degrees = 0.0;
    }
    const SDL_FlipMode sanitized_flip = static_cast<SDL_FlipMode>(
        static_cast<int>(flip) & (static_cast<int>(SDL_FLIP_HORIZONTAL) | static_cast<int>(SDL_FLIP_VERTICAL)));
    const bool changed =
        !anchor_sprite_transform_override_active_ ||
        anchor_sprite_transform_override_flip_ != sanitized_flip ||
        std::fabs(anchor_sprite_transform_override_angle_degrees_ - angle_degrees) > 1e-6;
    anchor_sprite_transform_override_active_ = true;
    anchor_sprite_transform_override_flip_ = sanitized_flip;
    anchor_sprite_transform_override_angle_degrees_ = angle_degrees;
    return changed;
}

bool Asset::clear_anchor_sprite_transform_override() {
    const bool changed = anchor_sprite_transform_override_active_ ||
                         anchor_sprite_transform_override_flip_ != SDL_FLIP_NONE ||
                         std::fabs(anchor_sprite_transform_override_angle_degrees_) > 1e-6;
    anchor_sprite_transform_override_active_ = false;
    anchor_sprite_transform_override_flip_ = SDL_FLIP_NONE;
    anchor_sprite_transform_override_angle_degrees_ = 0.0;
    return changed;
}

SDL_FlipMode Asset::effective_render_flip() const {
    int flip_bits = flipped ? static_cast<int>(SDL_FLIP_HORIZONTAL) : static_cast<int>(SDL_FLIP_NONE);
    if (anchor_sprite_transform_override_active_) {
        flip_bits ^= static_cast<int>(anchor_sprite_transform_override_flip_);
    }
    return static_cast<SDL_FlipMode>(
        flip_bits & (static_cast<int>(SDL_FLIP_HORIZONTAL) | static_cast<int>(SDL_FLIP_VERTICAL)));
}

double Asset::effective_render_angle() const {
    if (!anchor_sprite_transform_override_active_) {
        return 0.0;
    }
    return std::isfinite(anchor_sprite_transform_override_angle_degrees_)
        ? anchor_sprite_transform_override_angle_degrees_
        : 0.0;
}

void Assets::mark_active_assets_dirty() {
    require_assets_state(this).active_assets_dirty = true;
}

Asset* Assets::spawn_asset(const std::string& name, SDL_Point world_pos) {
    AssetsState& state = require_assets_state(this);
    auto failure_it = state.spawn_failures_remaining.find(name);
    if (failure_it != state.spawn_failures_remaining.end() && failure_it->second > 0) {
        --failure_it->second;
        return nullptr;
    }
    auto asset = test_child_asset_runtime::make_test_asset(name, world_pos.x, 0, world_pos.y, 0);
    return test_child_asset_runtime::attach_owned_asset(this, std::move(asset));
}

Asset* Assets::find_asset_by_stable_id(const std::string& id) const {
    const AssetsState& state = require_assets_state(this);
    for (const auto& asset : state.owned_assets) {
        if (!asset) {
            continue;
        }
        if (!asset->spawn_id.empty() && asset->spawn_id == id) {
            return asset.get();
        }
        if (asset->info && asset->info->name == id) {
            return asset.get();
        }
    }
    return nullptr;
}

bool Assets::contains_asset(const Asset* asset) const {
    if (!asset) {
        return false;
    }
    const AssetsState& state = require_assets_state(this);
    return std::any_of(state.owned_assets.begin(), state.owned_assets.end(), [&](const std::unique_ptr<Asset>& owned) {
        return owned.get() == asset;
    });
}

void Assets::schedule_removal(Asset* asset) {
    if (!asset) {
        return;
    }
    AssetsState& state = require_assets_state(this);
    g_anchor_specs.erase(asset);
    auto it = std::remove_if(state.owned_assets.begin(), state.owned_assets.end(),
                             [&](std::unique_ptr<Asset>& owned) {
                                 if (!owned || owned.get() != asset) {
                                     return false;
                                 }
                                 owned->set_assets(nullptr);
                                 return true;
                             });
    state.owned_assets.erase(it, state.owned_assets.end());
}

#endif // ENGINE_WORLD_TESTS
