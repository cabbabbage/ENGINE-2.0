#include "ui/menu_ui.hpp"
#include "rendering/render/engine_renderer.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

#include "ui/tinyfiledialogs.h"
#include "asset_loader.hpp"
#include "AssetsManager.hpp"
#include "utils/input.hpp"
#include "utils/log.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "app/frame_pacing.hpp"
#include "rendering/render/render_diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <utility>
#include <chrono>

namespace fs = std::filesystem;

namespace {
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

int env_int_clamped(const char* name, int default_value, int min_value, int max_value) {
        const int safe_min = std::min(min_value, max_value);
        const int safe_max = std::max(min_value, max_value);
        const int safe_default = std::clamp(default_value, safe_min, safe_max);
        if (!name || !*name) {
                return safe_default;
        }
        const char* raw = std::getenv(name);
        if (!raw || !*raw) {
                return safe_default;
        }
        try {
                const int value = std::stoi(raw);
                return std::clamp(value, safe_min, safe_max);
        } catch (...) {
                return safe_default;
        }
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

bool codex_playtest_input_enabled() {
        static const bool enabled = env_flag_enabled("VIBBLE_CODEX_PLAYTEST_INPUT", false);
        return enabled;
}
} // namespace

bool MenuUI::MenuPacketStructuralKey::operator==(const MenuPacketStructuralKey& other) const {
        return layout_version == other.layout_version &&
               theme_version == other.theme_version &&
               viewport_bucket == other.viewport_bucket &&
               feature_flags == other.feature_flags &&
               material_variant_id == other.material_variant_id;
}

namespace {
std::size_t hash_menu_packet_key(const MenuUI::MenuPacketStructuralKey& key) {
        std::size_t h = 0;
        auto mix = [&](std::size_t value) {
                h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        mix(std::hash<std::uint64_t>{}(key.layout_version));
        mix(std::hash<std::uint64_t>{}(key.theme_version));
        mix(std::hash<std::uint32_t>{}(key.viewport_bucket));
        mix(std::hash<std::uint32_t>{}(key.feature_flags));
        mix(std::hash<std::uint32_t>{}(key.material_variant_id));
        return h;
}
}

MenuUI::MenuUI(EngineRenderer* renderer,
               int screen_w,
               int screen_h,
               MapDescriptor map,
               LoadingScreen* loading_screen,
               AssetLibrary* asset_library,
               SDL_Window* window)
: MainApp(std::move(map), renderer, screen_w, screen_h, loading_screen, asset_library, window)
{
        if (TTF_WasInit() == 0) {
                if (!TTF_Init()) {
                        std::cerr << "TTF_Init failed: " << SDL_GetError() << "\n";
                }
	}
	menu_active_ = false;
}

MenuUI::~MenuUI() = default;

void MenuUI::init() {
        setup();
        rebuildButtons();
        game_loop();
}

bool MenuUI::wants_return_to_main_menu() const {
	return return_to_main_menu_;
}

void MenuUI::game_loop() {
        SDL_Renderer* renderer = raw_renderer();
        if (!renderer) {
                std::cerr << "[MenuUI] Renderer unavailable; exiting game loop.\n";
                return;
        }

	bool quit = false;
	SDL_Event e;
        return_to_main_menu_ = false;
        constexpr int EVENT_WAIT_TIMEOUT_MS = 16;
        std::uint64_t runtime_frame_counter = 0;
        const int auto_exit_frame_limit =
                env_int_clamped("VIBBLE_RUNTIME_FRAME_LIMIT", 0, 0, 1000000);
        const double perf_frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        const double target_counts = app::frame_pacing::target_frame_counts(perf_frequency);
        if (auto_exit_frame_limit > 0) {
                vibble::log::info("[MenuUI] Runtime frame limit: " + std::to_string(auto_exit_frame_limit));
        }
        if (codex_playtest_input_enabled()) {
                vibble::log::info("[CodexPlaytest] In-engine input driver enabled for MenuUI runtime.");
        }
        auto process_event = [&](const SDL_Event& event) {
                if (renderer && is_resize_or_scale_event(event.type)) {
                        const Uint64 resize_sync_begin = SDL_GetPerformanceCounter();
                        if (sync_output_dimensions(renderer)) {
                                rebuildButtons();
                        }
                        runtime_stats::FrameStatsRecorder::instance().add(
                                "main.resize_sync_ms",
                                runtime_stats::FrameStatsRecorder::elapsed_ms(resize_sync_begin,
                                                                              SDL_GetPerformanceCounter()));
                }
                handle_global_shortcuts(event);
                const bool menu_was_active = menu_active_;
                if (event.type == SDL_EVENT_QUIT) {
                        runtime_stats::FrameStatsRecorder::instance().mark_stage("shutdown_save_begin", true);
                        run_exit_save_sequence("sdl_event_quit");
                        runtime_stats::FrameStatsRecorder::instance().mark_stage("shutdown_save_end", true);
                        quit = true;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && event.key.repeat == 0) {
                        bool esc_consumed = false;
                        if (game_assets_) {
                                esc_consumed = game_assets_->consume_escape_for_asset_editor_stack();
                        }
                        if (!esc_consumed) {
                                toggleMenu();
                        }
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
                        const bool ctrl_down = (event.key.mod & SDL_KMOD_CTRL) != 0;
                        if (ctrl_down && event.key.key == SDLK_D) {
                                doToggleDevMode();
                        }
                }
                if (menu_active_) {
                        handle_event(event);
                        return;
                }
                if (menu_was_active) {
                        // Don't let the closing event fall through to gameplay handlers.
                        return;
                }
                if (input_) {
                        input_->handleEvent(event);
                }
                if (game_assets_) {
                        game_assets_->handle_sdl_event(event);
                }
        };
	while (!quit) {
                ++runtime_frame_counter;
                auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
                frame_stats.begin_frame(runtime_frame_counter);
                frame_stats.set("main.telemetry_schema", "freeze_watchdog_v1");
                struct RuntimeFrameScope {
                        runtime_stats::FrameStatsRecorder& stats;
                        bool active = true;

                        explicit RuntimeFrameScope(runtime_stats::FrameStatsRecorder& recorder)
                            : stats(recorder) {}

                        ~RuntimeFrameScope() {
                                if (active) {
                                        stats.end_frame();
                                }
                        }

                        void end() {
                                if (active) {
                                        stats.end_frame();
                                        active = false;
                                }
                        }
                } runtime_frame_scope(frame_stats);
                const Uint64 frame_begin = SDL_GetPerformanceCounter();
                int event_count = 0;
                const bool gameplay_active_before_events = !menu_active_ && game_assets_ && input_;
                const Uint64 event_begin = SDL_GetPerformanceCounter();
                if (gameplay_active_before_events) {
                        frame_stats.mark_stage("event_poll_begin");
                        while (SDL_PollEvent(&e)) {
                                ++event_count;
                                process_event(e);
                        }
                } else {
                        frame_stats.mark_stage("idle_wait", true);
                        if (SDL_WaitEventTimeout(&e, EVENT_WAIT_TIMEOUT_MS)) {
                                frame_stats.mark_stage("event_wait_returned");
                                ++event_count;
                                process_event(e);
                                frame_stats.mark_stage("event_poll_begin");
                                while (SDL_PollEvent(&e)) {
                                        ++event_count;
                                        process_event(e);
                                }
                        }
                }
                const Uint64 event_end = SDL_GetPerformanceCounter();
                frame_stats.mark_stage("event_poll_end");
                frame_stats.set("main.event_count", event_count);
                frame_stats.set("main.event_poll_ms",
                                runtime_stats::FrameStatsRecorder::elapsed_ms(event_begin, event_end));

                frame_stats.mark_stage("sync_output_begin");
                const Uint64 sync_begin = SDL_GetPerformanceCounter();
                if (sync_output_dimensions(renderer)) {
                        rebuildButtons();
                }
                frame_stats.set("main.sync_output_ms",
                                runtime_stats::FrameStatsRecorder::elapsed_ms(sync_begin,
                                                                              SDL_GetPerformanceCounter()));
                frame_stats.mark_stage("sync_output_end");

                const bool should_update = !menu_active_ && game_assets_ && input_;
                frame_stats.set("main.gameplay_active", should_update);
                if (should_update) {
                        if (codex_playtest_input_enabled()) {
                                frame_stats.mark_stage("codex_playtest_input_begin");
                                input_->applyCodexPlaytestDriverForTest(runtime_frame_counter,
                                                                        screen_w_,
                                                                        screen_h_);
                                frame_stats.mark_stage("codex_playtest_input_end");
                        }
                        frame_stats.mark_stage("assets_update_begin");
                        const Uint64 assets_begin = SDL_GetPerformanceCounter();
                        game_assets_->update(*input_);
                        frame_stats.set("main.assets_update_ms",
                                        runtime_stats::FrameStatsRecorder::elapsed_ms(assets_begin,
                                                                                      SDL_GetPerformanceCounter()));
                        frame_stats.mark_stage("assets_update_end");
                }
                if (menu_active_) {
                        frame_stats.mark_stage("menu_render_begin");
                        render();
                        frame_stats.mark_stage("menu_render_end");
                        frame_stats.mark_stage("menu_action_begin");
                        switch (consumeAction()) {
                                case MenuAction::EXIT:     doExit();    closeMenu(); quit = true; break;
                                case MenuAction::QUIT:     doQuit();    closeMenu(); quit = true; break;
                                case MenuAction::SETTINGS: doSettings();             break;
                                default: break;
                        }
                        frame_stats.mark_stage("menu_action_end");
                }

                if (!should_update) {
                        render_diagnostics::begin_frame();
                        render_diagnostics::set_renderer_runtime_info(
                                "menu_or_idle",
                                renderer_ ? renderer_->renderer_name() : "unknown",
                                renderer_ ? renderer_->present_mode_name() : "unknown");
                        render_diagnostics::set_render_stage_timings(menu_active_ ? "menu_only" : "idle_no_gameplay_update");
                        render_diagnostics::set_submit_result(false);
                        render_diagnostics::end_frame();
                }

                if (renderer_) {
                        frame_stats.mark_stage("present_begin");
                        const Uint64 present_begin = SDL_GetPerformanceCounter();
                        renderer_->present();
                        frame_stats.set("main.present_ms",
                                        runtime_stats::FrameStatsRecorder::elapsed_ms(present_begin,
                                                                                      SDL_GetPerformanceCounter()));
                        frame_stats.mark_stage("present_end");
                } else {
                        frame_stats.set("main.present_ms", 0.0);
                }
                frame_stats.mark_stage("render_diagnostics_begin");
                log_render_diagnostics(renderer, "MenuUI");
                frame_stats.mark_stage("render_diagnostics_end");

                if (input_) {
                        frame_stats.mark_stage("input_update_begin");
                        const Uint64 input_begin = SDL_GetPerformanceCounter();
                        input_->update();
                        frame_stats.set("main.input_update_ms",
                                        runtime_stats::FrameStatsRecorder::elapsed_ms(input_begin,
                                                                                      SDL_GetPerformanceCounter()));
                        frame_stats.mark_stage("input_update_end");
                }

                const double remaining_counts =
                        app::frame_pacing::remaining_frame_counts(frame_begin,
                                                                  target_counts,
                                                                  perf_frequency);
                const bool use_end_idle_pacing = should_update && remaining_counts > 0.0;
                frame_stats.set("main.idle_wait_used", use_end_idle_pacing);
                frame_stats.set("main.idle_pacing_requested_ms",
                                use_end_idle_pacing
                                        ? (remaining_counts * 1000.0) / perf_frequency
                                        : 0.0);
                if (use_end_idle_pacing) {
                        frame_stats.mark_stage("frame_pacing_begin");
                        const Uint64 idle_begin = SDL_GetPerformanceCounter();
                        app::frame_pacing::delay_from_remaining_counts(remaining_counts,
                                                                       perf_frequency);
                        frame_stats.set("main.idle_pacing_delay_ms",
                                        runtime_stats::FrameStatsRecorder::elapsed_ms(idle_begin,
                                                                                      SDL_GetPerformanceCounter()));
                        frame_stats.mark_stage("frame_pacing_end");
                } else {
                        frame_stats.set("main.idle_pacing_delay_ms", 0.0);
                }
                frame_stats.set("main.menu_active", menu_active_);
                if (auto_exit_frame_limit > 0 &&
                    runtime_frame_counter >= static_cast<std::uint64_t>(auto_exit_frame_limit)) {
                        quit = true;
                }
                frame_stats.set("main.quit_requested", quit);
                frame_stats.set("main.frame_total_ms",
                                runtime_stats::FrameStatsRecorder::elapsed_ms(frame_begin,
                                                                              SDL_GetPerformanceCounter()));
                runtime_frame_scope.end();
	}
        runtime_stats::FrameStatsRecorder::instance().shutdown();
        if (auto_exit_frame_limit > 0) {
                vibble::log::info("[MenuUI] Runtime frame limit reached; exiting automation run.");
                std::exit(0);
        }
}

void MenuUI::toggleMenu() {
        if (menu_active_) {
                closeMenu();
        } else {
                openMenu();
        }
        std::cout << "[MenuUI] ESC -> menu_active=" << (menu_active_ ? "true" : "false") << "\n";
}

void MenuUI::openMenu() {
        exitDevModeForPause();
        menu_active_ = true;
        if (game_assets_) game_assets_->set_render_suppressed(menu_active_);
}

void MenuUI::closeMenu() {
        if (!menu_active_) return;
        menu_active_ = false;
        if (game_assets_) game_assets_->set_render_suppressed(menu_active_);
}

void MenuUI::exitDevModeForPause() {
        if (!dev_mode_) return;
        dev_mode_ = false;
        if (game_assets_) game_assets_->set_dev_mode(dev_mode_);
        std::cout << "[MenuUI] Dev Mode disabled for pause menu\n";
        rebuildButtons();
}

void MenuUI::handle_event(const SDL_Event& e) {
	for (auto& mb : buttons_) {
		if (mb.button.handle_event(e)) {
			last_action_ = mb.action;
			std::cout << "[MenuUI] Button clicked: " << mb.button.text() << "\n";
		}
	}
}

void MenuUI::render() {
        SDL_Renderer* renderer = raw_renderer();
        if (!renderer) {
                return;
        }
        ++packet_frame_index_;
        const Uint64 build_begin = SDL_GetPerformanceCounter();
        const MenuPacketStructuralKey key = compute_packet_structural_key();
        const std::size_t key_hash = hash_menu_packet_key(key);
        auto cached = packet_cache_.find(key_hash);
        if (cached != packet_cache_.end() && cached->second.valid && cached->second.key == key) {
                packet_cache_stats_.cache_hit_count++;
                packet_cache_stats_.reused_packet_count++;
                cached->second.last_used_frame = packet_frame_index_;
                dynamic_packet_commands_ = cached->second.commands;
        } else {
                packet_cache_stats_.cache_miss_count++;
                MenuStructuralPacket packet{};
                packet.key = key;
                packet.creation_frame = packet_frame_index_;
                packet.last_used_frame = packet_frame_index_;
                packet.commands = build_structural_commands();
                packet.estimated_memory_cost = estimate_command_memory_cost(packet.commands);
                packet_cache_memory_footprint_ += packet.estimated_memory_cost;
                packet_cache_[key_hash] = std::move(packet);
                dynamic_packet_commands_ = packet_cache_[key_hash].commands;
                packet_cache_stats_.structural_rebuild_count++;
                trim_packet_cache_if_needed();
        }
        packet_cache_stats_.structural_build_ms_total += runtime_stats::FrameStatsRecorder::elapsed_ms(
                build_begin, SDL_GetPerformanceCounter());

        const Uint64 patch_begin = SDL_GetPerformanceCounter();
        for (const MenuDrawCommand& command : dynamic_packet_commands_) {
                switch (command.kind) {
                case MenuDrawCommand::Kind::BackgroundFill:
                        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                        SDL_SetRenderDrawColor(renderer, 0, 0, 0, command.alpha);
                        sdl_render::FillRect(renderer, &command.rect);
                        break;
                case MenuDrawCommand::Kind::Vignette:
                        drawVignette(command.alpha);
                        break;
                case MenuDrawCommand::Kind::TitleText:
                        blitTextCentered(renderer, Styles::LabelTitle(), command.text, command.rect, true, SDL_Color{0,0,0,0});
                        break;
                case MenuDrawCommand::Kind::Button:
                        if (command.button_index < buttons_.size()) {
                                buttons_[command.button_index].button.render(renderer);
                        } else {
                                invalidate_packet_cache_entry(key);
                        }
                        break;
                }
        }
        packet_cache_stats_.reuse_patch_ms_total += runtime_stats::FrameStatsRecorder::elapsed_ms(
                patch_begin, SDL_GetPerformanceCounter());
        emit_packet_cache_metrics();
}

MenuUI::MenuAction MenuUI::consumeAction() {
	MenuAction a = last_action_;
	last_action_ = MenuAction::NONE;
	return a;
}

void MenuUI::rebuildButtons() {
        buttons_.clear();
        const int btn_w = Button::width();
        const int btn_h = Button::height();
        const int gap   = 16;
        int start_y = 150;
        const int x = (screen_w_ - btn_w) / 2;
        auto addButton = [&](const std::string& label, MenuAction action, bool is_exit=false) {
                Button b = is_exit ? Button::get_exit_button(label) : Button::get_main_button(label);
                b.set_glass_style(Button::default_glass_style());
                b.enable_glass_style(true);
                b.set_rect(SDL_Rect{ x, start_y, btn_w, btn_h });
                start_y += btn_h + gap;
                buttons_.push_back(MenuButton{ std::move(b), action });
};
        addButton("End Run",            MenuAction::EXIT, true);
        addButton("Quit Game",          MenuAction::QUIT, true);
        addButton("Settings",           MenuAction::SETTINGS);
        ++packet_layout_version_;
        invalidate_packet_cache();
}

MenuUI::MenuPacketStructuralKey MenuUI::compute_packet_structural_key() const {
        MenuPacketStructuralKey key{};
        key.layout_version = packet_layout_version_;
        key.theme_version = packet_theme_version_;
        key.viewport_bucket = static_cast<std::uint32_t>((screen_w_ / 160) << 16 | (screen_h_ / 90));
        key.feature_flags = static_cast<std::uint32_t>(dev_mode_ ? 0x1u : 0u);
        key.material_variant_id = 0;
        return key;
}

std::vector<MenuUI::MenuDrawCommand> MenuUI::build_structural_commands() const {
        std::vector<MenuDrawCommand> commands{};
        commands.push_back(MenuDrawCommand{MenuDrawCommand::Kind::BackgroundFill, 0, SDL_Rect{0, 0, screen_w_, screen_h_}, "", 100});
        commands.push_back(MenuDrawCommand{MenuDrawCommand::Kind::Vignette, 0, SDL_Rect{0,0,0,0}, "", 110});
        commands.push_back(MenuDrawCommand{MenuDrawCommand::Kind::TitleText, 0, SDL_Rect{0, 60, screen_w_, 60}, "PAUSE MENU", 0});
        commands.reserve(commands.size() + buttons_.size());
        for (std::size_t i = 0; i < buttons_.size(); ++i) {
                commands.push_back(MenuDrawCommand{MenuDrawCommand::Kind::Button, i, SDL_Rect{0,0,0,0}, "", 0});
        }
        return commands;
}

std::size_t MenuUI::estimate_command_memory_cost(const std::vector<MenuDrawCommand>& commands) const {
        std::size_t total = 0;
        for (const MenuDrawCommand& c : commands) {
                total += sizeof(MenuDrawCommand) + c.text.capacity();
        }
        return total;
}
void MenuUI::trim_packet_cache_if_needed() {
        while (packet_cache_.size() > packet_cache_max_entries_ || packet_cache_memory_footprint_ > packet_cache_max_memory_bytes_) {
                auto victim = packet_cache_.end();
                for (auto it = packet_cache_.begin(); it != packet_cache_.end(); ++it) {
                        if (victim == packet_cache_.end() || it->second.last_used_frame < victim->second.last_used_frame) {
                                victim = it;
                        }
                }
                if (victim == packet_cache_.end()) break;
                packet_cache_memory_footprint_ -= std::min(packet_cache_memory_footprint_, victim->second.estimated_memory_cost);
                packet_cache_.erase(victim);
        }
}
void MenuUI::invalidate_packet_cache() {
        packet_cache_.clear();
        packet_cache_memory_footprint_ = 0;
}
void MenuUI::invalidate_packet_cache_entry(const MenuPacketStructuralKey& key) {
        const auto it = packet_cache_.find(hash_menu_packet_key(key));
        if (it != packet_cache_.end()) {
            it->second.valid = false;
        }
}
void MenuUI::emit_packet_cache_metrics() const {
        auto& stats = runtime_stats::FrameStatsRecorder::instance();
        stats.set("menu.packet_cache_hit_count", static_cast<double>(packet_cache_stats_.cache_hit_count));
        stats.set("menu.packet_cache_miss_count", static_cast<double>(packet_cache_stats_.cache_miss_count));
        stats.set("menu.packet_structural_rebuild_count", static_cast<double>(packet_cache_stats_.structural_rebuild_count));
        stats.set("menu.packet_reused_count", static_cast<double>(packet_cache_stats_.reused_packet_count));
        stats.set("menu.packet_cache_memory_bytes", static_cast<double>(packet_cache_memory_footprint_));
        const double build_avg = packet_cache_stats_.structural_rebuild_count
                                 ? packet_cache_stats_.structural_build_ms_total / static_cast<double>(packet_cache_stats_.structural_rebuild_count)
                                 : 0.0;
        const double reuse_avg = packet_cache_stats_.reused_packet_count
                                 ? packet_cache_stats_.reuse_patch_ms_total / static_cast<double>(packet_cache_stats_.reused_packet_count)
                                 : 0.0;
        stats.set("menu.packet_structural_build_avg_ms", build_avg);
        stats.set("menu.packet_reuse_patch_avg_ms", reuse_avg);
}

SDL_Point MenuUI::measureText(const LabelStyle& style, const std::string& s) const {
	SDL_Point sz{0,0};
	if (s.empty()) return sz;
	TTF_Font* f = style.open_font();
	if (!f) return sz;
	ttf_util::GetStringSize(f, s, &sz.x, &sz.y);
	TTF_CloseFont(f);
	return sz;
}

void MenuUI::blitText(SDL_Renderer* r,
                      const LabelStyle& style,
                      const std::string& s,
                      int x, int y,
                      bool shadow,
                      SDL_Color override_col) const
{
	if (s.empty()) return;
	TTF_Font* f = style.open_font();
	if (!f) return;
	const SDL_Color coal = Styles::Coal();
	const SDL_Color col  = override_col.a ? override_col : style.color;
	SDL_Surface* surf_text = ttf_util::RenderTextBlended(f, s, col);
	SDL_Surface* surf_shadow = shadow ? ttf_util::RenderTextBlended(f, s, coal) : nullptr;
	if (surf_text) {
		SDL_Texture* tex_text = SDL_CreateTextureFromSurface(r, surf_text);
		if (surf_shadow) {
			SDL_Texture* tex_shadow = SDL_CreateTextureFromSurface(r, surf_shadow);
			if (tex_shadow) {
					SDL_Rect dsts { x+2, y+2, surf_shadow->w, surf_shadow->h };
					SDL_SetTextureAlphaMod(tex_shadow, 130);
					sdl_render::Texture(r, tex_shadow, nullptr, &dsts);
					SDL_DestroyTexture(tex_shadow);
			}
		}
		if (tex_text) {
			SDL_Rect dst { x, y, surf_text->w, surf_text->h };
			sdl_render::Texture(r, tex_text, nullptr, &dst);
			SDL_DestroyTexture(tex_text);
		}
	}
	if (surf_shadow) SDL_DestroySurface(surf_shadow);
	if (surf_text)   SDL_DestroySurface(surf_text);
	TTF_CloseFont(f);
}

void MenuUI::blitTextCentered(SDL_Renderer* r,
                              const LabelStyle& style,
                              const std::string& s,
                              const SDL_Rect& rect,
                              bool shadow,
                              SDL_Color override_col) const
{
	SDL_Point sz = measureText(style, s);
	const int x = rect.x + (rect.w - sz.x)/2;
	const int y = rect.y + (rect.h - sz.y)/2;
	blitText(r, style, s, x, y, shadow, override_col);
}

void MenuUI::drawVignette(Uint8 alpha) const {
	SDL_Renderer* renderer = raw_renderer();
	if (!renderer) {
		return;
	}
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
	SDL_Rect v{0,0,screen_w_,screen_h_};
	sdl_render::FillRect(renderer, &v);
}

bool MenuUI::run_exit_save_sequence(const std::string& reason) {
        if (!game_assets_) {
                std::cout << "[MenuUI] Exit save sequence skipped (game assets unavailable, reason='"
                          << reason << "')\n";
                return true;
        }

        const bool ok = game_assets_->run_exit_save_sequence(reason);
        if (ok) {
                std::cout << "[MenuUI] Exit save sequence finished (reason='" << reason << "')\n";
        } else {
                std::cerr << "[MenuUI] EXIT SAVE FAILURE before shutdown (reason='" << reason
                          << "').\n";
        }
        return ok;
}

void MenuUI::doExit() {
        const bool saved = run_exit_save_sequence("pause_menu_end_run");
	std::cout << "[MenuUI] End Run -> return to main menu (exit_save="
                  << (saved ? "ok" : "FAILED") << ")\n";
	return_to_main_menu_ = true;
}

void MenuUI::doSettings() {
	std::cout << "[MenuUI] Settings opened\n";
}

void MenuUI::doQuit() {
        const bool saved = run_exit_save_sequence("pause_menu_quit_game");
	std::cout << "[MenuUI] Quit Game -> exiting application (exit_save="
                  << (saved ? "ok" : "FAILED") << ")\n";
}

void MenuUI::doToggleDevMode() {
        dev_mode_ = !dev_mode_;
        if (game_assets_) game_assets_->set_dev_mode(dev_mode_);
        std::cout << "[MenuUI] Dev Mode = " << (dev_mode_ ? "ON" : "OFF") << "\n";
        rebuildButtons();

        if (menu_active_) {
                menu_active_ = false;
                if (game_assets_) game_assets_->set_render_suppressed(false);
                std::cout << "[MenuUI] Closing menu after mode switch\n";
        }
}


