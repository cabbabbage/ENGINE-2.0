#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "gameplay/world/world_grid.hpp"
#include "runtime_world_context.hpp"

class Asset;
class AssetLoader;
class AssetLibrary;
class Assets;
class Input;
struct SDL_Renderer;
struct SDL_Window;

namespace app::bootstrap {

struct RuntimeBootstrapRequest {
    SDL_Renderer* renderer = nullptr;
    AssetLibrary* shared_asset_library = nullptr;
    const AssetLoader* source_loader = nullptr;
    std::string map_id;
    nlohmann::json map_manifest;
    std::string content_root;
    std::function<void(const std::string&)> status_notifier;
};

struct RuntimeBootstrapResult {
    std::unique_ptr<AssetLoader> loader;
    std::shared_ptr<RuntimeWorldContext> world_context;
    world::WorldGrid world_grid{};
    Asset* player_ptr = nullptr;
    int start_px = 0;
    int start_pz = 0;
    int camera_map_radius = 0;
    std::size_t asset_count = 0;
    std::size_t room_count = 0;
    long long loader_init_ms = 0;
    long long create_assets_ms = 0;
};

RuntimeBootstrapResult prepare_runtime_bootstrap(RuntimeBootstrapRequest request);

std::unique_ptr<Assets> create_assets_from_bootstrap(RuntimeBootstrapResult& bootstrap,
                                                     int screen_w,
                                                     int screen_h,
                                                     SDL_Renderer* renderer,
                                                     SDL_Window* window = nullptr);

void finalize_assets_post_init(Assets& assets,
                               std::unique_ptr<Input>& input,
                               bool& dev_mode,
                               Asset* player_ptr,
                               const std::function<void()>& on_missing_player = {});

template <typename WorkFn, typename ErrorFn>
void run_guarded_or_throw(WorkFn&& work, ErrorFn&& on_error) {
    try {
        std::forward<WorkFn>(work)();
    } catch (const std::exception& ex) {
        std::forward<ErrorFn>(on_error)(ex);
        throw;
    }
}

template <typename WorkFn, typename ErrorFn>
bool run_guarded(WorkFn&& work, ErrorFn&& on_error) {
    try {
        std::forward<WorkFn>(work)();
        return true;
    } catch (const std::exception& ex) {
        std::forward<ErrorFn>(on_error)(ex);
        return false;
    }
}

}  // namespace app::bootstrap
