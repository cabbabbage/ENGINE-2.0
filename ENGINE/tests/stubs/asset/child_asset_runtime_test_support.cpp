#include "child_asset_runtime_test_support.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_library.hpp"
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
    AssetLibrary* library = nullptr;
};

std::unordered_map<Assets*, AssetsState> g_assets_states;
struct AssetLibraryState {
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> info_by_name;
};
std::unordered_map<AssetLibrary*, AssetLibraryState> g_library_states;
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
    void* library_memory = ::operator new(sizeof(AssetLibrary), std::align_val_t(alignof(AssetLibrary)));
    auto* library = reinterpret_cast<AssetLibrary*>(library_memory);
    g_library_states.emplace(library, AssetLibraryState{});

    AssetsState state{};
    state.library = library;
    g_assets_states.emplace(assets, std::move(state));
    return assets;
}

void destroy_assets_stub(Assets* assets) {
    if (!assets) {
        return;
    }
    const auto state_it = g_assets_states.find(assets);
    if (state_it != g_assets_states.end() && state_it->second.library) {
        g_library_states.erase(state_it->second.library);
        ::operator delete(state_it->second.library, std::align_val_t(alignof(AssetLibrary)));
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
    if (state.library && asset->info && !asset->info->name.empty()) {
        g_library_states[state.library].info_by_name[asset->info->name] = asset->info;
    }
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

#endif // ENGINE_WORLD_TESTS