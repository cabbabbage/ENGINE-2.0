#include "ui/menu_ui.hpp"
#include "app/bootstrap.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

#include "ui/tinyfiledialogs.h"
#include "asset_loader.hpp"
#include "AssetsManager.hpp"
#include "utils/input.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <utility>

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
} // namespace

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

        constexpr double TARGET_FPS = 60.0;
        constexpr double TARGET_FRAME_SECONDS = 1.0 / TARGET_FPS;
        const double perf_frequency = static_cast<double>(SDL_GetPerformanceFrequency());
        const double target_counts  = TARGET_FRAME_SECONDS * perf_frequency;

        SDL_Renderer* renderer = raw_renderer();
        if (!renderer) {
                std::cerr << "[MenuUI] Renderer unavailable; exiting game loop.\n";
                return;
        }

	bool quit = false;
	SDL_Event e;
        return_to_main_menu_ = false;
        double idle_counts_accum = 0.0;
        int idle_frame_counter   = 0;
        constexpr int IDLE_REPORT_INTERVAL = 120;

	while (!quit) {
                const Uint64 frame_begin = SDL_GetPerformanceCounter();
                bool had_events = false;
                while (SDL_PollEvent(&e)) {
                        had_events = true;
                        if (renderer && is_resize_or_scale_event(e.type)) {
                                if (sync_output_dimensions(renderer)) {
                                        rebuildButtons();
                                }
                        }
                        handle_global_shortcuts(e);
                        const bool menu_was_active = menu_active_;
			if (e.type == SDL_EVENT_QUIT) {
                                run_exit_save_sequence("sdl_event_quit");
					quit = true;
			}
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && e.key.repeat == 0) {
                                bool esc_consumed = false;
                                if (game_assets_) {
                                        esc_consumed = game_assets_->consume_escape_for_asset_editor_stack();
                                }
                                if (!esc_consumed) {
                                        toggleMenu();
                                }
                        }
                        if (e.type == SDL_EVENT_KEY_DOWN && e.key.repeat == 0) {
                                const bool ctrl_down = (e.key.mod & SDL_KMOD_CTRL) != 0;
                                if (ctrl_down && e.key.key == SDLK_D) {
                                        doToggleDevMode();
                                }
                        }
                        if (menu_active_) {
                                handle_event(e);
                                continue;
                        }
                        if (menu_was_active) {
                                // Don't let the closing event fall through to gameplay handlers.
                                continue;
                        }
                        if (input_) input_->handleEvent(e);
                        if (game_assets_) game_assets_->handle_sdl_event(e);
                }

                const bool should_update = !menu_active_ && game_assets_ && input_;

                if (should_update) {
                        game_assets_->update(*input_);
                }

                if (sync_output_dimensions(renderer)) {
                        rebuildButtons();
                }
                log_render_diagnostics(renderer, "MenuUI");

                if (menu_active_) {
                        render();
                        switch (consumeAction()) {
                                case MenuAction::EXIT:     doExit();    closeMenu(); quit = true; break;
                                case MenuAction::RESTART:  doRestart(); closeMenu(); break;
                                case MenuAction::QUIT:     doQuit();    closeMenu(); quit = true; break;
                                case MenuAction::SETTINGS: doSettings();             break;
                                default: break;
                        }
                }

                SDL_RenderPresent(renderer);

                if (input_) input_->update();

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

                        idle_counts_accum = 0.0;
                        idle_frame_counter = 0;
                }
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
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 100);
	SDL_Rect bg{0, 0, screen_w_, screen_h_};
	sdl_render::FillRect(renderer, &bg);
	drawVignette(110);
	const std::string title = "PAUSE MENU";
	SDL_Rect trect{ 0, 60, screen_w_, 60 };
	blitTextCentered(renderer, Styles::LabelTitle(), title, trect, true, SDL_Color{0,0,0,0});
	for (auto& mb : buttons_) {
		mb.button.render(renderer);
	}
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
        addButton("Restart Run",        MenuAction::RESTART);
        addButton("Quit Game",          MenuAction::QUIT, true);
        addButton("Settings",           MenuAction::SETTINGS);
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

void MenuUI::doRestart() {
        std::cout << "[MenuUI] Restarting...\n";
        game_assets_.reset();
        SDL_Renderer* renderer = raw_renderer();
        if (!renderer) {
                std::cerr << "[MenuUI] Cannot restart without a renderer.\n";
                return;
        }
        if (!loader_) {
                std::cerr << "[MenuUI] Cannot restart before initial setup has completed.\n";
                return;
        }
        const bool restart_ok = app::bootstrap::run_guarded(
                [&]() {
                        app::bootstrap::RuntimeBootstrapRequest bootstrap_request;
                        bootstrap_request.renderer = renderer;
                        bootstrap_request.shared_asset_library = asset_library_;
                        bootstrap_request.source_loader = loader_.get();

                        auto bootstrap_result = app::bootstrap::prepare_runtime_bootstrap(std::move(bootstrap_request));
                        game_assets_ = app::bootstrap::create_assets_from_bootstrap(bootstrap_result,
                                                                                    screen_w_,
                                                                                    screen_h_,
                                                                                    renderer);
                        loader_ = std::move(bootstrap_result.loader);
                        app::bootstrap::finalize_assets_post_init(
                                *game_assets_,
                                input_,
                                dev_mode_,
                                bootstrap_result.player_ptr,
                                []() {
                                        std::cout << "[MenuUI] No player asset found. Launching in Dev Mode.\n";
                                });
                },
                [](const std::exception& ex) {
                        std::cerr << "[MenuUI] Restart failed: " << ex.what() << "\n";
                });
        if (!restart_ok) {
                return;
        }
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



