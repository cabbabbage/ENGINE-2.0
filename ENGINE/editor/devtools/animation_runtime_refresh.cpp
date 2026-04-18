#include "devtools/animation_runtime_refresh.hpp"

#include <atomic>
#include <unordered_set>
#include <string>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

namespace devmode {
namespace {

std::uint64_t next_refresh_transaction_id() {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}

bool has_valid_frame_binding(Asset* asset) {
    if (!asset || !asset->current_frame) {
        return false;
    }
    return asset->get_current_variant_texture() != nullptr;
}

void log_refresh_event(std::uint64_t transaction_id,
                       const Asset* asset,
                       const std::string& stage,
                       const std::string& detail) {
#if !defined(NDEBUG)
    const std::string asset_name =
        (asset && asset->info) ? asset->info->name : std::string{"<unknown>"};
    const std::string animation =
        asset ? (asset->current_animation.empty() ? std::string{"<none>"} : asset->current_animation)
              : std::string{"<none>"};
    const std::string message =
        std::string("[AnimRefresh][tx=") + std::to_string(transaction_id) +
        "] stage=" + stage +
        " asset=" + asset_name +
        " animation=" + animation +
        " detail=" + detail;
    vibble::log::debug(message);
#else
    (void)transaction_id;
    (void)asset;
    (void)stage;
    (void)detail;
#endif
}

void validate_refresh_invariants(std::uint64_t transaction_id,
                                 Asset* asset,
                                 const std::shared_ptr<AssetInfo>& info) {
    if (!asset || !info || asset->info.get() != info.get()) {
        return;
    }

    if (info->animations.empty()) {
        if (!asset->current_animation.empty() || asset->current_frame != nullptr) {
            vibble::log::warn(
                std::string("[AnimRefresh][tx=") + std::to_string(transaction_id) +
                "] asset has runtime frame state but no animations are available.");
        }
        return;
    }

    auto animation_it = info->animations.find(asset->current_animation);
    if (animation_it == info->animations.end()) {
        vibble::log::warn(
            std::string("[AnimRefresh][tx=") + std::to_string(transaction_id) +
            "] current_animation missing from info after refresh.");
        return;
    }

    if (animation_it->second.has_frames() && asset->current_frame == nullptr) {
        vibble::log::warn(
            std::string("[AnimRefresh][tx=") + std::to_string(transaction_id) +
            "] animation has frames but current_frame is null after refresh.");
    }

    if (asset->current_frame != nullptr &&
        animation_it->second.index_of(asset->current_frame) < 0) {
        vibble::log::warn(
            std::string("[AnimRefresh][tx=") + std::to_string(transaction_id) +
            "] current_frame does not belong to current_animation after refresh.");
    }

    if (asset->current_frame != nullptr && !has_valid_frame_binding(asset)) {
        vibble::log::warn(
            std::string("[AnimRefresh][tx=") + std::to_string(transaction_id) +
            "] current_frame has no bound texture after refresh.");
    }
}

} // namespace

void refresh_loaded_animation_instances(Assets* assets,
                                        const std::shared_ptr<AssetInfo>& info) {
    if (!assets || !info) {
        return;
    }

    const std::uint64_t transaction_id = next_refresh_transaction_id();
    std::unordered_set<Asset*> visited;
    auto refresh = [&](Asset* asset) {
        if (!asset || asset->info.get() != info.get()) {
            return;
        }
        if (!visited.insert(asset).second) {
            return;
        }

        const bool previous_hidden = asset->is_hidden();
        const bool previous_anchor_hidden = asset->is_anchor_hidden();
        log_refresh_event(transaction_id, asset, "prepare", "begin");

        asset->rebuild_animation_runtime();
        asset->current_frame = nullptr;
        asset->set_frame_progress(0.0f);
        asset->static_frame = false;

        std::string desired = asset->current_animation.empty()
                                   ? std::string{"default"}
                                   : asset->current_animation;
        auto it = info->animations.find(desired);
        if (it == info->animations.end()) {
            it = info->animations.find("default");
        }
        if (it == info->animations.end() && !info->animations.empty()) {
            it = info->animations.begin();
        }

        if (it != info->animations.end()) {
            asset->set_current_animation(it->first);
            auto& anim = it->second;
            asset->static_frame = anim.is_frozen() || anim.locked;
        } else {
            asset->current_animation.clear();
            asset->current_frame = nullptr;
        }

        asset->refresh_frame_texture_bindings();
        asset->set_hidden(previous_hidden);
        asset->set_anchor_hidden(previous_anchor_hidden);
        asset->on_scale_factor_changed();
        validate_refresh_invariants(transaction_id, asset, info);
        log_refresh_event(
            transaction_id,
            asset,
            "activate",
            has_valid_frame_binding(asset) ? "frame-bound" : "frame-missing-or-unbound");
};

    for (Asset* asset : assets->all) {
        refresh(asset);
    }

    assets->mark_active_assets_dirty();
}

}
