#include "main.hpp"
#include "utils/text_style.hpp"
#include "ui/main_menu.hpp"
#include "ui/menu_ui.hpp"
#include "ui/tinyfiledialogs.h"
#include "ui/loading_screen.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "core/manifest/map_manifest_normalizer.hpp"
#include "asset_loader.hpp"
#include "assets/asset/asset_types.hpp"
#include "assets/asset/asset_library.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/engine_renderer.hpp"
#include "AssetsManager.hpp"
#include "utils/input.hpp"
#include "audio/audio_engine.hpp"
#include "devtools/core/manifest_store.hpp"
#include "utils/loading_status_notifier.hpp"
#include "gameplay/world/world_grid.hpp"
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <optional>
#include <iomanip>
#include <cctype>
#include <system_error>
#include <utility>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include "utils/log.hpp"

namespace fs = std::filesystem;

namespace {

struct WindowedPlacement {
        int x;
        int y;
        int w;
        int h;
};

WindowedPlacement compute_windowed_fallback(SDL_Window* window) {
        WindowedPlacement placement{SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720};

        if (!window) {
                return placement;
        }

        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        if (display != 0) {
                if (const SDL_DisplayMode* desktop_mode = SDL_GetDesktopDisplayMode(display)) {
                        const int margin = 120;
                        const int preferred_w = (desktop_mode->w * 3) / 4;
                        const int preferred_h = (desktop_mode->h * 3) / 4;

                        const int max_w = std::max(640, desktop_mode->w - margin);
                        const int max_h = std::max(360, desktop_mode->h - margin);

                        placement.w = std::max(960, preferred_w);
                        placement.h = std::max(540, preferred_h);

                        placement.w = std::min(placement.w, max_w);
                        placement.h = std::min(placement.h, max_h);
                }
        }

        return placement;
}

}

#if defined(_WIN32)
extern "C" {
        __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
        __declspec(dllexport) int NvOptimusEnablement                = 0x00000001;
}
#endif

MainApp::MainApp(MapDescriptor map,
                 EngineRenderer* renderer,
                 int screen_w,
                 int screen_h,
                 LoadingScreen* loading_screen,
                 AssetLibrary* asset_library,
                 SDL_Window* window)
: map_descriptor_(std::move(map)),
  map_path_(map_descriptor_.id),
  renderer_(renderer),
  screen_w_(screen_w),
  screen_h_(screen_h),
  loading_screen_(loading_screen),
  asset_library_(asset_library),
  window_(window),
  is_fullscreen_(window_ ? ((SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0) : false) {
        if (window_) {
                if (!is_fullscreen_) {
                        int width = 0;
                        int height = 0;
                        SDL_GetWindowSize(window_, &width, &height);
                        if (width > 0 && height > 0) {
                                windowed_width_ = width;
                                windowed_height_ = height;
                        }
                        SDL_GetWindowPosition(window_, &windowed_x_, &windowed_y_);
                } else {
                        const WindowedPlacement fallback = compute_windowed_fallback(window_);
                        windowed_x_ = fallback.x;
                        windowed_y_ = fallback.y;
                        windowed_width_ = fallback.w;
                        windowed_height_ = fallback.h;
                }
        }
}

MainApp::~MainApp() {
        AudioEngine::instance().shutdown();
        if (overlay_texture_)  SDL_DestroyTexture(overlay_texture_);
        delete game_assets_;
        delete input_;
}

SDL_Renderer* MainApp::raw_renderer() const {
        return renderer_ ? renderer_->raw() : nullptr;
}

void MainApp::init() {
        setup();
        vibble::log::info("[MainApp] Loading pipeline complete. Entering main loop...");
        game_loop();
}

void MainApp::setup() {
        std::srand(static_cast<unsigned int>(std::time(nullptr)));

        SDL_Renderer* renderer = raw_renderer();

        try {

                struct ScopedLogLevel {
                        vibble::log::Level prev;
                        explicit ScopedLogLevel(vibble::log::Level next)
                        : prev(vibble::log::level()) {
                                vibble::log::set_level(next);
                        }
                        ~ScopedLogLevel() {
                                vibble::log::set_level(prev);
                        }
};

                std::unique_ptr<ScopedLogLevel> loader_debug_guard;
                if (const char* v = std::getenv("VIBBLE_LOADER_DEBUG")) {
                        if (*v && (*v == '1' || *v == 'y' || *v == 'Y' ||
                                   *v == 't' || *v == 'T' ||
                                   std::tolower(static_cast<unsigned char>(*v)) == 'd')) {
                                loader_debug_guard = std::make_unique<ScopedLogLevel>(vibble::log::Level::Debug);
                                vibble::log::info("[MainApp] VIBBLE_LOADER_DEBUG enabled; log level set to DEBUG during loading.");
                        }
                }

                std::unique_ptr<loading_status::ScopedNotifier> scoped_loading_notifier;
                if (loading_screen_ && renderer) {
                        scoped_loading_notifier = std::make_unique<loading_status::ScopedNotifier>(
                                [this, renderer](const std::string& status) {
                                        try {
                                                loading_screen_->set_status(status);
                                                loading_screen_->draw_frame();
                                                SDL_RenderPresent(renderer);
                                        } catch (...) {

                                        }
                                        SDL_Event ev;
                                        while (SDL_PollEvent(&ev)) {

                                        }
                                });

                        loading_screen_->set_status("Preparing...");
                        loading_screen_->draw_frame();
                        SDL_RenderPresent(renderer);
                        SDL_Event ev;
                        while (SDL_PollEvent(&ev)) {}
                }

                std::string content_root;
                const std::string map_identifier = map_descriptor_.id.empty() ? map_path_ : map_descriptor_.id;

                manifest::ManifestData manifest_data = manifest::load_manifest();
                const nlohmann::json* fallback_manifest =
                        (map_descriptor_.data.is_object() && !map_descriptor_.data.empty())
                                ? &map_descriptor_.data
                                : nullptr;
                manifest::MapManifestBootstrapResult bootstrap = manifest::bootstrap_map_manifest(
                        manifest_data, map_identifier, fallback_manifest);
                nlohmann::json map_manifest_json = std::move(bootstrap.map_manifest);
                const fs::path resolved_root = bootstrap.resolved_content_root;
                const bool manifest_updated = bootstrap.changed;
                if (!bootstrap.manifest_entry_found && fallback_manifest) {
                        vibble::log::warn(std::string("[MainApp] Map '") + map_identifier + "' missing from manifest. Using descriptor payload.");
                } else if (!bootstrap.manifest_entry_found) {
                        vibble::log::warn(std::string("[MainApp] Map '") + map_identifier + "' missing from manifest. Deferring to normalization defaults.");
                }
                if (bootstrap.changed && !bootstrap.manifest_entry_found) {
                        vibble::log::warn(std::string("[MainApp] Map '") + map_identifier + "' missing from manifest. Applying normalized defaults.");
                } else if (bootstrap.changed) {
                        vibble::log::warn(std::string("[MainApp] Normalized manifest defaults for map '") + map_identifier + "'.");
                }

                std::error_code dir_error;
                fs::create_directories(resolved_root, dir_error);
                if (dir_error) {
                        std::ostringstream oss;
                        oss << "Failed to prepare content root '" << resolved_root.string() << "': " << dir_error.message();
                        throw std::runtime_error(oss.str());
                }
                content_root = resolved_root.string();

                if (manifest_updated) {
                        try {
                                devmode::core::ManifestStore store;
                                store.reload();
                                devmode::core::ManifestStore::MapPersistOptions options;
                                options.flush = true;
                                options.guard_reason = "MainApp::content_root_rewrite";
                                if (!store.persist_map_entry(map_identifier, map_manifest_json, options)) {
                                        vibble::log::warn(std::string("[MainApp] Failed to persist manifest entry for '") + map_identifier + "'.");
                                }
                        } catch (const std::exception& ex) {
                                vibble::log::warn(std::string("[MainApp] Unable to persist manifest entry for '") + map_identifier + "': " + ex.what());
                        }
                }

                vibble::log::info("[MainApp] Constructing AssetLoader...");
                auto loader_begin = std::chrono::steady_clock::now();
                loader_ = std::make_unique<AssetLoader>( map_identifier, map_manifest_json, renderer, content_root, nullptr, asset_library_);
                auto loader_end = std::chrono::steady_clock::now();
                vibble::log::info( std::string("[MainApp] AssetLoader constructed in ") + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>( loader_end - loader_begin) .count()) + "ms");

                loading_status::notify("Spawning assets");
                auto spawn_begin = std::chrono::steady_clock::now();
                world::WorldGrid world_grid{};
                loader_->createAssets(world_grid);
                auto all_assets = world_grid.all_assets();
                vibble::log::info(std::string("[MainApp] Asset spawning finished for map '") + map_identifier + "'.");
                vibble::log::info(std::string("[MainApp] ") + std::to_string(all_assets.size()) + " assets created and cached.");

                if (all_assets.size() > 200000) {
                        vibble::log::error(std::string("[MainApp] Asset count ") + std::to_string(all_assets.size()) + " exceeds limit (200000). Aborting to avoid instability.");
                        throw std::runtime_error("Asset count exceeds 200000; aborting.");
                }

                const auto asset_count = all_assets.size();
                const auto room_count = loader_->getRooms().size();

                Asset* player_ptr = nullptr;
                for (Asset* candidate : all_assets) {
                        if (candidate && candidate->info &&
                            candidate->info->type == asset_types::player) {
                                player_ptr = candidate;
                                break;
                        }
                }

                int start_px = player_ptr ? player_ptr->world_x()
                                          : static_cast<int>(loader_->getMapRadius());
                int start_pz = player_ptr ? player_ptr->world_z()
                                          : static_cast<int>(loader_->getMapRadius());

                AssetLibrary* active_library = loader_->getAssetLibrary();
                if (!active_library) {
                        throw std::runtime_error("Asset library unavailable during game setup.");
                }

                vibble::log::info("[MainApp] Creating Assets object...");
                game_assets_ = new Assets( *active_library, player_ptr, loader_->getRooms(), screen_w_, screen_h_, start_px, start_pz, static_cast<int>(loader_->getMapRadius() * 1.2), renderer, loader_->map_identifier(), loader_->map_manifest(), loader_->content_root(), std::move(world_grid));
                vibble::log::info("[MainApp] Assets object created successfully.");

                const double spawn_seconds =
                        std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - spawn_begin) .count() / 1000.0;

                std::ostringstream init_summary;
                init_summary << "[Init] Assets initialized: " << asset_count
                             << " assets across " << room_count << " rooms in "
                             << std::fixed << std::setprecision(2) << spawn_seconds << "s";
                vibble::log::info(init_summary.str());

                input_ = new Input();
                game_assets_->set_input(input_);
                if (game_assets_) {
                        game_assets_->reload_camera_settings();
                }
                if (!player_ptr) {
                        dev_mode_ = true;
                        vibble::log::warn("[MainApp] No player asset found. Launching in Dev Mode.");
                }
                if (game_assets_) {
                        game_assets_->set_dev_mode(dev_mode_);
                        // Apply camera settings AFTER dev_mode is set to ensure correct initialization
                        game_assets_->apply_camera_runtime_settings();
                        game_assets_->force_camera_view_refresh();
                }
                AudioEngine::instance().update();
        } catch (const std::exception& e) {
                vibble::log::error(std::string("[MainApp] Setup error: ") + e.what());
                throw;
        }
}

void MainApp::game_loop() {
        constexpr double TARGET_FPS = 60.0;
        constexpr double TARGET_FRAME_SECONDS = 1.0 / TARGET_FPS;
        const double perf_frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        const double target_counts  = TARGET_FRAME_SECONDS * perf_frequency;

        double idle_counts_accum = 0.0;
        int idle_frame_counter   = 0;
        constexpr int IDLE_REPORT_INTERVAL = 120;
        bool quit = false;
        SDL_Event e;

        vibble::log::info("[MainApp] Game loop started.");

        while (!quit) {
                const Uint64 frame_begin = SDL_GetPerformanceCounter();

                while (SDL_PollEvent(&e)) {
                        handle_global_shortcuts(e);
                        if (e.type == SDL_EVENT_QUIT) {
                                quit = true;
                        }
                        if (input_) {
                                input_->handleEvent(e);
                        }
                        if (game_assets_) {
                                game_assets_->handle_sdl_event(e);
                        }
                }

                // Pause updates while in dev mode unless something actually needs a tick.
                const bool dev_idle = dev_mode_
                        && game_assets_ && input_
                        && !game_assets_->should_step_dev_frame(*input_);

                if (!dev_idle && game_assets_ && input_) {
                        game_assets_->update(*input_);
                } else if (dev_idle && game_assets_) {
                        game_assets_->touch_last_frame_counter();
                }
                if (input_) {
                        input_->update();
                }

                const Uint64 frame_end = SDL_GetPerformanceCounter();
                const double work_counts = static_cast<double>(frame_end - frame_begin);

                if (work_counts < target_counts) {
                        const double remaining_counts = target_counts - work_counts;
                        idle_counts_accum += remaining_counts;
                        ++idle_frame_counter;
                        const double remaining_ms = (remaining_counts * 1000.0) / perf_frequency;
                        if (remaining_ms >= 1.0) {
                                SDL_Delay(static_cast<Uint32>(remaining_ms));
                        }
                }

                if (idle_frame_counter >= IDLE_REPORT_INTERVAL) {
                        const double total_idle_ms =
                                (idle_counts_accum * 1000.0) / perf_frequency;
                        const double average_idle_ms =
                                total_idle_ms / static_cast<double>(idle_frame_counter);
                        vibble::log::debug("[MainApp] Idle pacing: total " + std::to_string(total_idle_ms) + "ms over " + std::to_string(idle_frame_counter) + " frame(s); avg " + std::to_string(average_idle_ms) + "ms.");
                        idle_counts_accum = 0.0;
                        idle_frame_counter = 0;
                }
        }
}

void MainApp::toggle_fullscreen() {
        if (!window_) {
                return;
        }

        if (is_fullscreen_) {
                WindowedPlacement target{windowed_x_, windowed_y_, windowed_width_, windowed_height_};

                if (target.w <= 0 || target.h <= 0) {
                        target = compute_windowed_fallback(window_);
                } else {
                        const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
                        if (display != 0) {
                                if (const SDL_DisplayMode* desktop_mode = SDL_GetDesktopDisplayMode(display)) {
                                        if (target.w >= desktop_mode->w - 16 || target.h >= desktop_mode->h - 16) {
                                                target = compute_windowed_fallback(window_);
                                        }
                                }
                        }
                }

                const bool result = SDL_SetWindowFullscreen(window_, false);
                if (!result) {
                        vibble::log::warn(std::string("[MainApp] Failed to switch to windowed mode: ") + SDL_GetError());
                        return;
                }

                SDL_SetWindowResizable(window_, true);
                SDL_SetWindowBordered(window_, true);
                SDL_SetWindowSize(window_, target.w, target.h);
                SDL_SetWindowPosition(window_, target.x, target.y);

                is_fullscreen_ = false;
                windowed_x_ = target.x;
                windowed_y_ = target.y;
                windowed_width_ = target.w;
                windowed_height_ = target.h;
                vibble::log::info("[MainApp] Window mode switched to windowed.");
                return;
        }

        int current_x = 0;
        int current_y = 0;
        int current_width = 0;
        int current_height = 0;
        SDL_GetWindowPosition(window_, &current_x, &current_y);
        SDL_GetWindowSize(window_, &current_width, &current_height);
        if (current_width > 0 && current_height > 0) {
                windowed_x_ = current_x;
                windowed_y_ = current_y;
                windowed_width_ = current_width;
                windowed_height_ = current_height;
        }

        const bool result = SDL_SetWindowFullscreen(window_, true);
        if (!result) {
                vibble::log::warn(std::string("[MainApp] Failed to switch to fullscreen mode: ") + SDL_GetError());
                return;
        }

        is_fullscreen_ = true;
        vibble::log::info("[MainApp] Window mode switched to fullscreen.");
}

void MainApp::handle_global_shortcuts(const SDL_Event& e) {
        if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat != 0) {
                return;
        }
        const SDL_Keycode key = e.key.key;
        if (key != SDLK_RETURN && key != SDLK_KP_ENTER) {
                return;
        }
        if ((e.key.mod & SDL_KMOD_ALT) == 0) {
                return;
        }
        toggle_fullscreen();
}

namespace {

std::string trim_copy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string lowercase_identifier(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char ch : value) {
        unsigned char uc = static_cast<unsigned char>(ch);
        if (std::isalnum(uc) || ch == '_' || ch == '-') {
            normalized.push_back(static_cast<char>(std::tolower(uc)));
        } else {
            return std::string{};
        }
    }
    return normalized;
}

std::optional<std::string> sanitize_map_name(const std::string& input) {
    std::string trimmed = trim_copy(input);
    if (trimmed.empty()) {
        return std::nullopt;
    }
        return lowercase_identifier(trimmed);
}

std::optional<MapDescriptor> create_new_map_interactively() {
    devmode::core::ManifestStore manifest_store;
    try {
        manifest_store.reload();
    } catch (const std::exception& ex) {
        std::string msg = std::string("Failed to load manifest:\n") + ex.what();
        tinyfd_messageBox("Error", msg.c_str(), "ok", "error", 0);
        return std::nullopt;
    }

    while (true) {
        const char* response =
            tinyfd_inputBox("Create New Map", "Enter the name for your new map:", "");
        if (!response) {
            return std::nullopt;
        }

        auto sanitized = sanitize_map_name(response);
        if (!sanitized) {
            tinyfd_messageBox("Invalid Map Name", "Map names may only contain letters, numbers, underscores, or hyphens.", "ok", "error", 0);
            continue;
        }

        if (manifest_store.find_map_entry(*sanitized)) {
            tinyfd_messageBox("Map Exists", "A map with that name already exists.", "ok", "error", 0);
            continue;
        }

        nlohmann::json map_info = manifest::build_default_map_manifest(*sanitized);

        fs::path manifest_root;
        try {
            manifest_root = fs::absolute(fs::path(manifest::manifest_path()).parent_path());
        } catch (const std::exception& ex) {
            std::string msg = std::string("Unable to determine project root: ") + ex.what();
            tinyfd_messageBox("Error", msg.c_str(), "ok", "error", 0);
            continue;
        }

        std::error_code dir_error;
        fs::path content_root = manifest_root / "content";
        fs::create_directories(content_root, dir_error);
        if (dir_error) {
            std::string msg = std::string("Failed to prepare content folder: ") + dir_error.message();
            tinyfd_messageBox("Error", msg.c_str(), "ok", "error", 0);
            continue;
        }

        fs::path map_dir = content_root / *sanitized;
        dir_error.clear();
        fs::create_directories(map_dir, dir_error);
        if (dir_error) {
            std::string msg = std::string("Failed to create map folder: ") + dir_error.message();
            tinyfd_messageBox("Error", msg.c_str(), "ok", "error", 0);
            continue;
        }
        dir_error.clear();
        fs::create_directories(map_dir / "music", dir_error);

        map_info["content_root"] =
            (fs::path("content") / *sanitized).generic_string();
        auto audio_it = map_info.find("audio");
        if (audio_it == map_info.end() || !audio_it->is_object()) {
            map_info["audio"] = nlohmann::json::object();
            audio_it = map_info.find("audio");
        }
        nlohmann::json& audio_section = (*audio_it);
        nlohmann::json& music_section = audio_section["music"];
        if (!music_section.is_object()) {
            music_section = nlohmann::json::object();
        }
        music_section["content_root"] =
            (fs::path(map_info["content_root"].get<std::string>()) / "music") .generic_string();
        if (!music_section.contains("tracks") ||
            !music_section["tracks"].is_array()) {
            music_section["tracks"] = nlohmann::json::array();
        }

        devmode::core::ManifestStore::MapPersistOptions options;
        options.flush = true;
        options.guard_reason = "MainApp::create_map";
        if (!manifest_store.persist_map_entry(*sanitized, map_info, options)) {
            tinyfd_messageBox("Error Creating Map", "Failed to update manifest for new map.", "ok", "error", 0);
            continue;
        }

        MapDescriptor descriptor;
        descriptor.id   = *sanitized;
        descriptor.data = std::move(map_info);
        return descriptor;
    }
}

}

void run(SDL_Window* window,
         EngineRenderer& engine_renderer,
         int screen_w,
         int screen_h) {
    (void)window;

    SDL_Renderer* renderer = engine_renderer.raw();

    if (renderer) {
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {}
    }

    manifest::ManifestData manifest_data;
    try {
        manifest_data = manifest::load_manifest();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {}
    } catch (const std::exception& ex) {
        vibble::log::error(std::string("[Main] Failed to load manifest: ") + ex.what());
        return;
    }

    std::shared_ptr<AssetLibrary> shared_asset_library =
        std::make_shared<AssetLibrary>(false);
    vibble::log::info("[Main] Preparing asset metadata cache...");
                shared_asset_library->load_all_from_resources();
    { SDL_Event ev; while (SDL_PollEvent(&ev)) {} }
    vibble::log::info(std::string("[Main] Asset metadata cache ready for ") + std::to_string(shared_asset_library->all().size()) + " asset(s).");
    vibble::log::info("[Main] Loading cached asset resources...");
    shared_asset_library->loadAllAnimations(renderer);
    { SDL_Event ev; while (SDL_PollEvent(&ev)) {} }
    vibble::log::info("[Main] Cached asset resources loaded.");

    std::optional<MapDescriptor> autostart_map;
    if (const char* env = std::getenv("VIBBLE_AUTOSTART_MAP")) {
        std::string desired = env;
        if (!desired.empty() && manifest_data.maps.is_object()) {
            auto it = manifest_data.maps.find(desired);
            if (it == manifest_data.maps.end() && !manifest_data.maps.empty()) {
                vibble::log::warn(std::string("[Main] Auto-start map '") + desired + "' not found; using first available map.");
                it = manifest_data.maps.begin();
            }
            if (it != manifest_data.maps.end()) {
                MapDescriptor descriptor;
                descriptor.id = it.key();
                descriptor.data = it.value();
                autostart_map = std::move(descriptor);
            } else {
                vibble::log::warn(std::string("[Main] VIBBLE_AUTOSTART_MAP requested '") + desired + "' but no maps are available.");
            }
        }
    }

    while (true) {
        std::optional<MapDescriptor> chosen_map;
        bool quit_requested = false;
        bool should_show_loading_screen = false;
        std::unique_ptr<MainMenu> menu;

        if (autostart_map) {
            chosen_map = std::move(autostart_map);
            should_show_loading_screen = true;
            vibble::log::info(std::string("[Main] Auto-selecting map via VIBBLE_AUTOSTART_MAP: ") + chosen_map->id);
        } else {
            menu = std::make_unique<MainMenu>(renderer, screen_w, screen_h, manifest_data.maps);
            vibble::log::info("[Main] Main menu displayed.");
            SDL_Event e;
            bool choosing = true;
            while (choosing) {
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_EVENT_QUIT) {
                        quit_requested = true;
                        choosing = false;
                        break;
                    }
                    auto result = menu->handle_event(e);
                    if (!result) {
                        continue;
                    }
                    if (result->id == "QUIT") {
                        quit_requested = true;
                        choosing = false;
                        break;
                    }
                    if (result->id == "CREATE_NEW_MAP") {
                        auto created = create_new_map_interactively();
                        if (created) {
                            chosen_map = std::move(*created);
                            vibble::log::info(std::string("[Main] New map created and selected: ") + chosen_map->id);
                            choosing = false;
                            should_show_loading_screen = true;
                        }
                        continue;
                    }
                    MapDescriptor descriptor;
                    descriptor.id   = result->id;
                    descriptor.data = result->data;
                    chosen_map = std::move(descriptor);
                    vibble::log::info(std::string("[Main] Map selected: ") + chosen_map->id);
                    choosing = false;
                    should_show_loading_screen = true;
                    break;
                }
                if (!choosing) {
                    break;
                }
                SDL_SetRenderTarget(renderer, nullptr);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                menu->render();
                SDL_RenderPresent(renderer);
                SDL_Delay(16);
            }
        }

        if (should_show_loading_screen && menu) {
            menu->showLoadingScreen();
        }
        if (quit_requested || !chosen_map) {
            break;
        }

        MapDescriptor selected_map = std::move(*chosen_map);
        LoadingScreen loading_screen(renderer, screen_w, screen_h);
        loading_screen.init();

        MenuUI app(&engine_renderer, screen_w, screen_h, std::move(selected_map), &loading_screen, shared_asset_library.get(), window);
        app.init();
        if (app.wants_return_to_main_menu()) {
            continue;
        }
        break;
    }
}

int main(int argc, char* argv[]) {
        (void)argc;
        (void)argv;
        vibble::log::info("[Main] Starting game engine...");
        vibble::log::info("[Main] Startup uses existing asset caches; missing/stale cache entries regenerate on demand.");

        const SDL_InitFlags init_flags =
                static_cast<SDL_InitFlags>(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
        if (!SDL_Init(init_flags)) {
                vibble::log::error(std::string("SDL_Init failed: ") + SDL_GetError());
                return 1;
        }

        if (!TTF_Init()) {
                vibble::log::error(std::string("TTF_Init failed: ") + SDL_GetError());
                SDL_Quit();
                return 1;
        }

        int window_width = 1280;
        int window_height = 720;
        const SDL_DisplayID primary_display = SDL_GetPrimaryDisplay();
        if (primary_display != 0) {
                if (const SDL_DisplayMode* desktop_mode = SDL_GetDesktopDisplayMode(primary_display)) {
                        window_width = desktop_mode->w;
                        window_height = desktop_mode->h;
                }
        }

        SDL_PropertiesID window_props = SDL_CreateProperties();
        if (!window_props ||
            !SDL_SetStringProperty(window_props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Game Window") ||
            !SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window_width) ||
            !SDL_SetNumberProperty(window_props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window_height) ||
            !SDL_SetBooleanProperty(window_props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true) ||
            !SDL_SetBooleanProperty(window_props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true)) {
                vibble::log::error(std::string("SDL_CreateWindow properties failed: ") + SDL_GetError());
                if (window_props) SDL_DestroyProperties(window_props);
                TTF_Quit();
                SDL_Quit();
                return 1;
        }

        SDL_Window* window = SDL_CreateWindowWithProperties(window_props);
        SDL_DestroyProperties(window_props);
        if (!window) {
                vibble::log::error(std::string("SDL_CreateWindowWithProperties failed: ") + SDL_GetError());
                TTF_Quit();
                SDL_Quit();
                return 1;
        }

        std::unique_ptr<EngineRenderer> engine_renderer = EngineRenderer::Create(window, true);
        if (!engine_renderer) {
                vibble::log::error("[Main] Failed to initialize renderer after all fallbacks.");
                SDL_DestroyWindow(window);
                TTF_Quit();
                SDL_Quit();
                return 1;
        }

        SDL_Renderer* renderer = engine_renderer->raw();
        if (!renderer) {
                vibble::log::error("[Main] EngineRenderer returned null SDL_Renderer.");
                engine_renderer.reset();
                SDL_DestroyWindow(window);
                TTF_Quit();
                SDL_Quit();
                return 1;
        }

        switch (engine_renderer->quality_tier()) {
        case RenderQualityTier::GPU:
                vibble::log::info("[Main] Render quality tier: GPU (full effects).");
                break;
        case RenderQualityTier::Accelerated:
                vibble::log::info("[Main] Render quality tier: Accelerated (reduced effects).");
                break;
        case RenderQualityTier::Software:
                vibble::log::info("[Main] Render quality tier: Software (minimal effects).");
                break;
        }

        int screen_width = window_width;
        int screen_height = window_height;
        if (!SDL_GetCurrentRenderOutputSize(renderer, &screen_width, &screen_height)) {
                vibble::log::warn(std::string("[Main] Unable to query renderer output size: ") + SDL_GetError());
                SDL_GetWindowSizeInPixels(window, &screen_width, &screen_height);
        }
        vibble::log::info(std::string("[Main] Screen resolution: ") + std::to_string(screen_width) + "x" + std::to_string(screen_height));

        run(window, *engine_renderer, screen_width, screen_height);

        engine_renderer.reset();
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        vibble::log::info("[Main] Game exited cleanly.");
        return 0;
}
