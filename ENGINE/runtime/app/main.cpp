#include "main.hpp"
#include "app/bootstrap.hpp"
#include "app/frame_pacing.hpp"
#include "utils/text_style.hpp"
#include "ui/main_menu.hpp"
#include "ui/menu_ui.hpp"
#include "ui/tinyfiledialogs.h"
#include "ui/loading_screen.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "core/manifest/map_manifest_normalizer.hpp"
#include "asset_loader.hpp"
#include "assets/asset/asset_library.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/engine_renderer.hpp"
#include "AssetsManager.hpp"
#include "utils/input.hpp"
#include "audio/audio_engine.hpp"
#include "devtools/core/manifest_store.hpp"
#include "utils/loading_status_notifier.hpp"
#include "utils/string_utils.hpp"
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

bool env_flag_enabled(const char* name, bool default_value) {
        if (!name || !*name) {
                return default_value;
        }
        const char* raw = std::getenv(name);
        if (!raw || !*raw) {
                return default_value;
        }

        std::string value(raw);
        std::transform(value.begin(),
                       value.end(),
                       value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (value == "1" || value == "true" || value == "yes" || value == "on" || value == "y" || value == "t") {
                return true;
        }
        if (value == "0" || value == "false" || value == "no" || value == "off" || value == "n" || value == "f") {
                return false;
        }
        return default_value;
}

bool is_resize_or_scale_event(Uint32 event_type) {
        switch (event_type) {
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
#ifdef SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
#endif
#ifdef SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED
        case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
#endif
                return true;
        default:
                return false;
        }
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
        render_diagnostics_enabled_ = env_flag_enabled("VIBBLE_RENDER_DIAGNOSTICS", false);
}

MainApp::~MainApp() {
        AudioEngine::instance().shutdown();
        if (overlay_texture_)  SDL_DestroyTexture(overlay_texture_);
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

        app::bootstrap::run_guarded_or_throw(
                [&]() {

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
                        manifest::MapManifestBootstrapResult bootstrap = manifest::bootstrap_map_manifest(
                                manifest_data, map_identifier);
                        nlohmann::json map_manifest_json = std::move(bootstrap.map_manifest);
                        const fs::path resolved_root = bootstrap.resolved_content_root;
                        const bool manifest_updated = bootstrap.changed;
                        if (!bootstrap.manifest_entry_found) {
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
                        app::bootstrap::RuntimeBootstrapRequest bootstrap_request;
                        bootstrap_request.renderer = renderer;
                        bootstrap_request.shared_asset_library = asset_library_;
                        bootstrap_request.map_id = map_identifier;
                        bootstrap_request.map_manifest = std::move(map_manifest_json);
                        bootstrap_request.content_root = content_root;
                        bootstrap_request.status_notifier = [](const std::string& status) {
                                loading_status::notify(status);
                        };

                        auto bootstrap_result = app::bootstrap::prepare_runtime_bootstrap(std::move(bootstrap_request));
                        vibble::log::info(std::string("[MainApp] AssetLoader constructed in ") + std::to_string(bootstrap_result.loader_init_ms) + "ms");
                        vibble::log::info(std::string("[MainApp] Asset spawning finished for map '") + map_identifier + "'.");
                        vibble::log::info(std::string("[MainApp] ") + std::to_string(bootstrap_result.asset_count) + " assets created and cached.");

                        vibble::log::info("[MainApp] Creating Assets object...");
                        game_assets_ = app::bootstrap::create_assets_from_bootstrap(bootstrap_result,
                                                                                    screen_w_,
                                                                                    screen_h_,
                                                                                    renderer);
                        loader_ = std::move(bootstrap_result.loader);
                        world_context_ = bootstrap_result.world_context;
                        vibble::log::info("[MainApp] Assets object created successfully.");

                        const double spawn_seconds = static_cast<double>(bootstrap_result.create_assets_ms) / 1000.0;

                        std::ostringstream init_summary;
                        init_summary << "[Init] Assets initialized: " << bootstrap_result.asset_count
                                     << " assets across " << bootstrap_result.room_count << " rooms in "
                                     << std::fixed << std::setprecision(2) << spawn_seconds << "s";
                        vibble::log::info(init_summary.str());

                        app::bootstrap::finalize_assets_post_init(
                                *game_assets_,
                                input_,
                                dev_mode_,
                                bootstrap_result.player_ptr,
                                []() {
                                        vibble::log::warn("[MainApp] No player asset found. Launching in Dev Mode.");
                                });
                        AudioEngine::instance().update();
                },
                [](const std::exception& e) {
                        vibble::log::error(std::string("[MainApp] Setup error: ") + e.what());
                });
}

void MainApp::game_loop() {
        const double perf_frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        const double target_counts  = app::frame_pacing::target_frame_counts(perf_frequency);

        double idle_counts_accum = 0.0;
        int idle_frame_counter   = 0;
        constexpr int IDLE_REPORT_INTERVAL = 120;
        bool quit = false;
        SDL_Event e;

        vibble::log::info("[MainApp] Game loop started.");
        vibble::log::info("[MainApp] Frame pacing target: " + app::frame_pacing::target_summary());

        while (!quit) {
                const Uint64 frame_begin = SDL_GetPerformanceCounter();
                SDL_Renderer* renderer = raw_renderer();

                while (SDL_PollEvent(&e)) {
                        if (renderer) {
                                // Keep event coordinates aligned with renderer-space hit testing (DPI/scaling aware).
                                SDL_ConvertEventToRenderCoordinates(renderer, &e);
                        }
                        if (renderer && is_resize_or_scale_event(e.type)) {
                                sync_output_dimensions(renderer);
                        }
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

                if (renderer) {
                        sync_output_dimensions(renderer);
                }
                if (game_assets_ && input_) {
                        game_assets_->update(*input_);
                }
                if (renderer) {
                        log_render_diagnostics(renderer, "MainApp");
                }
                if (input_) {
                        input_->update();
                }

                const double remaining_counts =
                        app::frame_pacing::remaining_frame_counts(frame_begin,
                                                                  target_counts,
                                                                  perf_frequency);
                if (remaining_counts > 0.0) {
                        idle_counts_accum += remaining_counts;
                        ++idle_frame_counter;
                        app::frame_pacing::delay_from_remaining_counts(remaining_counts,
                                                                       perf_frequency);
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

bool MainApp::sync_output_dimensions(SDL_Renderer* renderer) {
        if (!renderer) {
                return false;
        }

        int output_w = screen_w_;
        int output_h = screen_h_;
        if (!SDL_GetCurrentRenderOutputSize(renderer, &output_w, &output_h) || output_w <= 0 || output_h <= 0) {
                if (window_) {
                        SDL_GetWindowSizeInPixels(window_, &output_w, &output_h);
                }
        }
        output_w = std::max(1, output_w);
        output_h = std::max(1, output_h);
        if (output_w == screen_w_ && output_h == screen_h_) {
                return false;
        }

        screen_w_ = output_w;
        screen_h_ = output_h;
        if (game_assets_) {
                game_assets_->set_output_dimensions(screen_w_, screen_h_);
        }
        return true;
}

void MainApp::log_render_diagnostics(SDL_Renderer* renderer, const char* loop_label) {
        if (!render_diagnostics_enabled_ || !renderer) {
                return;
        }

        ++frame_diagnostics_counter_;

        int window_w = 0;
        int window_h = 0;
        int window_px_w = 0;
        int window_px_h = 0;
        if (window_) {
                SDL_GetWindowSize(window_, &window_w, &window_h);
                SDL_GetWindowSizeInPixels(window_, &window_px_w, &window_px_h);
        }

        int output_w = 0;
        int output_h = 0;
        if (!SDL_GetCurrentRenderOutputSize(renderer, &output_w, &output_h)) {
                output_w = screen_w_;
                output_h = screen_h_;
        }

        SDL_Texture* active_target = SDL_GetRenderTarget(renderer);
        int target_w = output_w;
        int target_h = output_h;
        bool target_is_backbuffer = (active_target == nullptr);
        if (active_target) {
                float tw = 0.0f;
                float th = 0.0f;
                if (SDL_GetTextureSize(active_target, &tw, &th) && tw > 0.0f && th > 0.0f) {
                        target_w = static_cast<int>(std::lround(tw));
                        target_h = static_cast<int>(std::lround(th));
                }
        }

        SDL_Rect viewport{0, 0, 0, 0};
        SDL_Rect clip{0, 0, 0, 0};
        SDL_GetRenderViewport(renderer, &viewport);
        SDL_GetRenderClipRect(renderer, &clip);

        int projection_w = 0;
        int projection_h = 0;
        float visible_l = 0.0f;
        float visible_t = 0.0f;
        float visible_r = 0.0f;
        float visible_b = 0.0f;
        std::uint32_t depth_culled = 0;
        int frustum_min_z = 0;
        int frustum_max_z = 0;
        int frustum_nodes = 0;
        int frustum_skipped = 0;
        std::string postprocess_text = "inactive";
        if (game_assets_) {
                const WarpedScreenGrid& cam = game_assets_->getView();
                const world::CameraProjectionParams params = cam.projection_params();
                projection_w = params.screen_width;
                projection_h = params.screen_height;
                const auto bounds = cam.get_bounds();
                visible_l = bounds.left;
                visible_t = bounds.top;
                visible_r = bounds.right;
                visible_b = bounds.bottom;
                depth_culled = cam.last_depth_culled();
                frustum_min_z = cam.last_min_world_z();
                frustum_max_z = cam.last_max_world_z();
                frustum_nodes = static_cast<int>(cam.last_nodes_visited());
                frustum_skipped = static_cast<int>(cam.last_branches_skipped());
                if (const std::optional<SDL_Point> pp_size = game_assets_->scene_postprocess_target_size()) {
                        postprocess_text = std::to_string(pp_size->x) + "x" + std::to_string(pp_size->y);
                }
        }

        std::ostringstream frame_line;
        frame_line << "[RenderDiag][" << (loop_label ? loop_label : "loop")
                   << "] frame=" << frame_diagnostics_counter_
                   << " window=" << window_w << "x" << window_h
                   << " window_px=" << window_px_w << "x" << window_px_h
                   << " output=" << output_w << "x" << output_h
                   << " target=" << target_w << "x" << target_h << (target_is_backbuffer ? "(backbuffer)" : "(texture)")
                   << " viewport=(" << viewport.x << "," << viewport.y << "," << viewport.w << "," << viewport.h << ")"
                   << " clip=(" << clip.x << "," << clip.y << "," << clip.w << "," << clip.h << ")"
                   << " projection=" << projection_w << "x" << projection_h
                   << " visible_rect=(" << visible_l << "," << visible_t << "," << visible_r << "," << visible_b << ")"
                   << " frustum_z=[" << frustum_min_z << "," << frustum_max_z << "]"
                   << " depth_culled=" << depth_culled
                   << " nodes=" << frustum_nodes
                   << " branches_skipped=" << frustum_skipped
                   << " postprocess=" << postprocess_text;
        vibble::log::debug(frame_line.str());

        if (output_h > 0 && std::abs(target_h - output_h / 2) <= 1) {
                vibble::log::warn("[RenderDiag] target height is half of output height (possible mismatch).");
        }
        if (viewport.y != 0) {
                vibble::log::warn("[RenderDiag] viewport has non-zero Y offset: " + std::to_string(viewport.y));
        }
        if (clip.y != 0) {
                vibble::log::warn("[RenderDiag] clip rect has non-zero Y offset: " + std::to_string(clip.y));
        }
        if (projection_w > 0 && projection_h > 0 && (projection_w != output_w || projection_h != output_h)) {
                vibble::log::warn("[RenderDiag] camera projection size does not match render output.");
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

std::optional<std::string> sanitize_map_name(const std::string& input) {
    const std::string trimmed = vibble::strings::trim_copy(input);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    for (char ch : trimmed) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (!std::isalnum(uc) && ch != '_' && ch != '-') {
            return std::nullopt;
        }
    }

    return vibble::strings::to_lower_copy(trimmed);
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
    const double perf_frequency = static_cast<double>(SDL_GetPerformanceFrequency());
    const double target_counts = app::frame_pacing::target_frame_counts(perf_frequency);
    vibble::log::info("[Main] Shared frame pacing target: " + app::frame_pacing::target_summary());

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
    const bool safe_loading_enabled = env_flag_enabled("VIBBLE_SAFE_LOADING", true);
    if (!safe_loading_enabled) {
        vibble::log::info("[Main] Loading cached asset resources...");
        shared_asset_library->loadAllAnimations(renderer);
        { SDL_Event ev; while (SDL_PollEvent(&ev)) {} }
        vibble::log::info("[Main] Cached asset resources loaded.");
    } else {
        vibble::log::info("[Main] SAFE loading is enabled; deferring full animation preload until runtime requests it.");
    }

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
                const Uint64 frame_begin = SDL_GetPerformanceCounter();
                while (SDL_PollEvent(&e)) {
                    if (renderer) {
                        // Keep menu pointer input in renderer-space coordinates.
                        SDL_ConvertEventToRenderCoordinates(renderer, &e);
                    }
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
                const double remaining_counts =
                    app::frame_pacing::remaining_frame_counts(frame_begin,
                                                              target_counts,
                                                              perf_frequency);
                app::frame_pacing::delay_from_remaining_counts(remaining_counts,
                                                               perf_frequency);
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
        vibble::log::info("[Main] Startup uses existing asset caches; missing cache entries regenerate on demand.");

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
