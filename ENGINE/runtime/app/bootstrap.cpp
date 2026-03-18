#include "app/bootstrap.hpp"

#include "asset_loader.hpp"
#include "assets/asset/asset_types.hpp"
#include "AssetsManager.hpp"
#include "utils/input.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace app::bootstrap {

namespace {

constexpr std::size_t kMaxAssetCount = 200000;

Asset* find_player(const std::vector<Asset*>& assets) {
    for (Asset* candidate : assets) {
        if (candidate && candidate->info && candidate->info->type == asset_types::player) {
            return candidate;
        }
    }
    return nullptr;
}

}  // namespace

RuntimeBootstrapResult prepare_runtime_bootstrap(RuntimeBootstrapRequest request) {
    if (!request.renderer) {
        throw std::runtime_error("Renderer unavailable during runtime bootstrap.");
    }

    if (request.source_loader) {
        request.map_id = request.source_loader->map_identifier();
        request.map_manifest = request.source_loader->map_manifest();
        request.content_root = request.source_loader->content_root();
    }

    if (request.map_id.empty()) {
        throw std::runtime_error("Map identifier missing during runtime bootstrap.");
    }

    RuntimeBootstrapResult result;
    const auto loader_begin = std::chrono::steady_clock::now();
    result.loader = std::make_unique<AssetLoader>(request.map_id,
                                                  request.map_manifest,
                                                  request.renderer,
                                                  request.content_root,
                                                  nullptr,
                                                  request.shared_asset_library);
    const auto loader_end = std::chrono::steady_clock::now();
    result.loader_init_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(loader_end - loader_begin).count();

    if (request.status_notifier) {
        request.status_notifier("Spawning assets");
    }

    const auto spawn_begin = std::chrono::steady_clock::now();
    result.loader->createAssets(result.world_grid);
    const auto spawn_end = std::chrono::steady_clock::now();
    result.create_assets_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(spawn_end - spawn_begin).count();

    const auto all_assets = result.world_grid.all_assets();
    result.asset_count = all_assets.size();
    if (result.asset_count > kMaxAssetCount) {
        throw std::runtime_error("Asset count exceeds 200000; aborting.");
    }

    result.room_count = result.loader->getRooms().size();
    result.player_ptr = find_player(all_assets);

    const int map_radius = static_cast<int>(result.loader->getMapRadius());
    result.start_px = result.player_ptr ? result.player_ptr->world_x() : map_radius;
    result.start_pz = result.player_ptr ? result.player_ptr->world_z() : map_radius;
    result.camera_map_radius = static_cast<int>(result.loader->getMapRadius() * 1.2);
    return result;
}

std::unique_ptr<Assets> create_assets_from_bootstrap(RuntimeBootstrapResult& bootstrap,
                                                     int screen_w,
                                                     int screen_h,
                                                     SDL_Renderer* renderer) {
    if (!bootstrap.loader) {
        throw std::runtime_error("Loader unavailable while creating assets.");
    }
    AssetLibrary* active_library = bootstrap.loader->getAssetLibrary();
    if (!active_library) {
        throw std::runtime_error("Asset library unavailable during runtime setup.");
    }
    return std::make_unique<Assets>(*active_library,
                                    bootstrap.player_ptr,
                                    bootstrap.loader->getRooms(),
                                    screen_w,
                                    screen_h,
                                    bootstrap.start_px,
                                    bootstrap.start_pz,
                                    bootstrap.camera_map_radius,
                                    renderer,
                                    bootstrap.loader->map_identifier(),
                                    bootstrap.loader->map_manifest(),
                                    bootstrap.loader->content_root(),
                                    std::move(bootstrap.world_grid));
}

void finalize_assets_post_init(Assets& assets,
                               std::unique_ptr<Input>& input,
                               bool& dev_mode,
                               Asset* player_ptr,
                               const std::function<void()>& on_missing_player) {
    if (!input) {
        input = std::make_unique<Input>();
    }
    assets.set_input(input.get());
    assets.reload_camera_settings();
    if (!player_ptr) {
        dev_mode = true;
        if (on_missing_player) {
            on_missing_player();
        }
    }
    assets.set_dev_mode(dev_mode);
    assets.apply_camera_runtime_settings();
    assets.force_camera_view_refresh();
}

}  // namespace app::bootstrap
