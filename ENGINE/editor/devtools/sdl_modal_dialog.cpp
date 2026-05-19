#include "devtools/sdl_modal_dialog.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/widgets.hpp"

namespace devmode::dialogs {
namespace {

constexpr int kDialogWidth = 520;
constexpr int kTextDialogHeight = 245;
constexpr int kChoiceDialogHeight = 225;
constexpr int kPadding = 22;
constexpr int kButtonGap = 10;

void draw_text(SDL_Renderer* renderer, const DMLabelStyle& style, const std::string& text, int x, int y) {
    if (!renderer || text.empty()) {
        return;
    }
    DMFontCache::instance().draw_text(renderer, style, text, x, y);
}

std::vector<std::string> wrap_text(const DMLabelStyle& style, const std::string& text, int max_width) {
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines;
    }

    auto append_wrapped_line = [&](const std::string& raw_line) {
        std::string current;
        std::string word;
        auto flush_word = [&]() {
            if (word.empty()) {
                return;
            }
            std::string candidate = current.empty() ? word : current + " " + word;
            if (DMFontCache::instance().measure_text(style, candidate).x <= max_width || current.empty()) {
                current = std::move(candidate);
            } else {
                lines.push_back(current);
                current = word;
            }
            word.clear();
        };

        for (char ch : raw_line) {
            if (ch == ' ') {
                flush_word();
            } else {
                word.push_back(ch);
            }
        }
        flush_word();
        lines.push_back(current);
    };

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t next = text.find('\n', start);
        append_wrapped_line(text.substr(start, next == std::string::npos ? std::string::npos : next - start));
        if (next == std::string::npos) {
            break;
        }
        start = next + 1;
    }

    return lines;
}

struct ModalWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    ~ModalWindow() {
        if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        if (window) {
            SDL_DestroyWindow(window);
        }
    }
};

ModalWindow create_modal_window(SDL_Window* parent, const std::string& title, int width, int height) {
    ModalWindow result;
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        return result;
    }

    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_UTILITY_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, true);
    if (parent) {
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, parent);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_MODAL_BOOLEAN, true);
    }

    result.window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!result.window) {
        return result;
    }

    result.renderer = SDL_CreateRenderer(result.window, nullptr);
    SDL_ShowWindow(result.window);
    SDL_SetWindowAlwaysOnTop(result.window, true);
    SDL_RaiseWindow(result.window);
    if (parent) {
        SDL_RaiseWindow(parent);
        SDL_RaiseWindow(result.window);
    }
    return result;
}

void render_dialog_frame(SDL_Renderer* renderer, const std::string& title, const std::string& message, int width, int height) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 24, 26, 30, 255);
    SDL_RenderClear(renderer);

    SDL_Rect panel{10, 10, width - 20, height - 20};
    dm_draw::DrawBeveledRect(renderer,
                             panel,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, panel, DMStyles::CornerRadius(), 1, DMStyles::Border());

    DMLabelStyle title_style = DMStyles::Label();
    title_style.font_size = 16;
    draw_text(renderer, title_style, title, kPadding, kPadding);

    DMLabelStyle body_style = DMStyles::Label();
    body_style.font_size = 13;
    const auto lines = wrap_text(body_style, message, width - kPadding * 2);
    int y = kPadding + 34;
    for (const std::string& line : lines) {
        draw_text(renderer, body_style, line, kPadding, y);
        y += body_style.font_size + 6;
        if (y > height - 76) {
            break;
        }
    }
}

void fallback_message_box(SDL_Window* parent,
                          const std::string& title,
                          const std::string& message,
                          MessageIcon icon) {
    SDL_MessageBoxFlags flags = SDL_MESSAGEBOX_INFORMATION;
    if (icon == MessageIcon::Warning) {
        flags = SDL_MESSAGEBOX_WARNING;
    } else if (icon == MessageIcon::Error) {
        flags = SDL_MESSAGEBOX_ERROR;
    }
    SDL_ShowSimpleMessageBox(flags, title.c_str(), message.c_str(), parent);
}

struct FileDialogState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::vector<std::filesystem::path> paths;
};

void SDLCALL file_dialog_callback(void* userdata, const char* const* filelist, int count) {
    auto* state = static_cast<FileDialogState*>(userdata);
    if (!state) {
        return;
    }

    SDL_Log("[SDLModalDialog] file_dialog_callback invoked: count=%d filelist=%s", count, filelist ? "set" : "null");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        int parsed_count = 0;
        if (!filelist) {
            // Fast cancel path: SDL reports cancellation via null list.
        } else if (count > 0) {
            const char* first_path = filelist[0];
            SDL_Log("[SDLModalDialog] file_dialog_callback first path: %s", (first_path && *first_path) ? first_path : "<empty>");
            for (int i = 0; i < count; ++i) {
                const char* path = filelist[i];
                if (path && *path) {
                    state->paths.emplace_back(path);
                    ++parsed_count;
                }
            }
        } else {
            for (int i = 0; filelist[i] != nullptr; ++i) {
                const char* path = filelist[i];
                if (path && *path) {
                    state->paths.emplace_back(path);
                    ++parsed_count;
                }
            }
        }
        SDL_Log("[SDLModalDialog] file_dialog_callback parsed paths: raw_count=%d parsed_count=%d", count, parsed_count);
        state->done = true;
    }
    state->cv.notify_one();
}

std::vector<std::filesystem::path> run_file_dialog(SDL_Window* parent,
                                                   SDL_FileDialogType type,
                                                   const std::string& title,
                                                   const std::filesystem::path& default_location,
                                                   const std::vector<FileDialogFilter>& filters,
                                                   bool allow_many) {
    raise_parent(parent);

    std::vector<SDL_DialogFileFilter> sdl_filters;
    sdl_filters.reserve(filters.size());
    for (const auto& filter : filters) {
        sdl_filters.push_back(SDL_DialogFileFilter{filter.name.c_str(), filter.pattern.c_str()});
    }

    FileDialogState state;
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        return {};
    }

    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    if (parent) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);
    }
    if (!default_location.empty()) {
        const std::string location = default_location.string();
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
    }
    if (!sdl_filters.empty()) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, sdl_filters.data());
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(sdl_filters.size()));
    }
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, allow_many);

    SDL_ShowFileDialogWithProperties(type, file_dialog_callback, &state, props);

    std::unique_lock<std::mutex> lock(state.mutex);
    while (!state.done) {
        lock.unlock();
        SDL_PumpEvents();
        lock.lock();
        state.cv.wait_for(lock, std::chrono::milliseconds(16));
    }
    std::vector<std::filesystem::path> paths = std::move(state.paths);
    lock.unlock();
    SDL_DestroyProperties(props);
    raise_parent(parent);
    return paths;
}

}  // namespace

void raise_parent(SDL_Window* parent) {
    if (!parent) {
        return;
    }
    SDL_ShowWindow(parent);
    SDL_RaiseWindow(parent);
    SDL_FlashWindow(parent, SDL_FLASH_BRIEFLY);
}

std::optional<int> show_choice(SDL_Window* parent,
                               const std::string& title,
                               const std::string& message,
                               const std::vector<DialogButton>& buttons,
                               MessageIcon icon) {
    if (buttons.empty()) {
        show_message(parent, title, message, icon);
        return std::nullopt;
    }

    raise_parent(parent);
    ModalWindow modal = create_modal_window(parent, title, kDialogWidth, kChoiceDialogHeight);
    if (!modal.window || !modal.renderer) {
        fallback_message_box(parent, title, message, icon);
        return std::nullopt;
    }

    std::vector<DMButton> widgets;
    widgets.reserve(buttons.size());
    int total_width = 0;
    for (const auto& button : buttons) {
        const DMButtonStyle& style = button.destructive ? DMStyles::DeleteButton() : DMStyles::HeaderButton();
        widgets.emplace_back(button.label, &style, 0, DMButton::height());
        const int width = std::max(110, widgets.back().preferred_width() + 24);
        total_width += width;
    }
    total_width += kButtonGap * std::max(0, static_cast<int>(widgets.size()) - 1);
    int x = std::max(kPadding, kDialogWidth - kPadding - total_width);
    const int y = kChoiceDialogHeight - kPadding - DMButton::height();
    for (auto& widget : widgets) {
        const int width = std::max(110, widget.preferred_width() + 24);
        widget.set_rect(SDL_Rect{x, y, width, DMButton::height()});
        x += width + kButtonGap;
    }

    bool done = false;
    std::optional<int> result;
    while (!done) {
        SDL_Event event;
        while (SDL_WaitEventTimeout(&event, 16)) {
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
                break;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(modal.window)) {
                done = true;
                break;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    auto it = std::find_if(buttons.begin(), buttons.end(), [](const DialogButton& button) {
                        return button.escape_default;
                    });
                    if (it != buttons.end()) {
                        result = it->id;
                    }
                    done = true;
                    break;
                }
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                    auto it = std::find_if(buttons.begin(), buttons.end(), [](const DialogButton& button) {
                        return button.return_default;
                    });
                    if (it != buttons.end()) {
                        result = it->id;
                        done = true;
                        break;
                    }
                }
            }
            for (std::size_t i = 0; i < widgets.size(); ++i) {
                if (widgets[i].handle_event(event) &&
                    event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                    event.button.button == SDL_BUTTON_LEFT) {
                    result = buttons[i].id;
                    done = true;
                    break;
                }
            }
            if (done) {
                break;
            }
        }

        render_dialog_frame(modal.renderer, title, message, kDialogWidth, kChoiceDialogHeight);
        for (auto& widget : widgets) {
            widget.render(modal.renderer);
        }
        SDL_RenderPresent(modal.renderer);
    }

    raise_parent(parent);
    return result;
}

bool confirm(SDL_Window* parent,
             const std::string& title,
             const std::string& message,
             const std::string& confirm_label,
             const std::string& cancel_label,
             bool destructive) {
    const auto result = show_choice(parent,
                                    title,
                                    message,
                                    {
                                        DialogButton{0, cancel_label, false, true, false},
                                        DialogButton{1, confirm_label, true, false, destructive},
                                    },
                                    destructive ? MessageIcon::Warning : MessageIcon::Info);
    return result && *result == 1;
}

void show_message(SDL_Window* parent,
                  const std::string& title,
                  const std::string& message,
                  MessageIcon icon) {
    (void)show_choice(parent,
                      title,
                      message,
                      {DialogButton{0, "OK", true, true, icon == MessageIcon::Error}},
                      icon);
}

std::optional<std::string> prompt_text(SDL_Window* parent,
                                       const std::string& title,
                                       const std::string& label,
                                       const std::string& initial_value) {
    raise_parent(parent);
    ModalWindow modal = create_modal_window(parent, title, kDialogWidth, kTextDialogHeight);
    if (!modal.window || !modal.renderer) {
        return std::nullopt;
    }

    DMTextBox input(label, initial_value);
    input.set_rect(SDL_Rect{kPadding, 96, kDialogWidth - kPadding * 2, input.preferred_height(kDialogWidth - kPadding * 2)});
    DMButton cancel("Cancel", &DMStyles::HeaderButton(), 120, DMButton::height());
    DMButton ok("OK", &DMStyles::CreateButton(), 120, DMButton::height());
    const int button_y = kTextDialogHeight - kPadding - DMButton::height();
    ok.set_rect(SDL_Rect{kDialogWidth - kPadding - 120, button_y, 120, DMButton::height()});
    cancel.set_rect(SDL_Rect{kDialogWidth - kPadding - 120 - kButtonGap - 120, button_y, 120, DMButton::height()});
    input.start_editing();

    bool done = false;
    bool accepted = false;
    while (!done) {
        SDL_Event event;
        while (SDL_WaitEventTimeout(&event, 16)) {
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
                break;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(modal.window)) {
                done = true;
                break;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    done = true;
                    break;
                }
                if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                    accepted = true;
                    done = true;
                    break;
                }
            }
            (void)input.handle_event(event);
            if (cancel.handle_event(event) &&
                event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                event.button.button == SDL_BUTTON_LEFT) {
                done = true;
                break;
            }
            if (ok.handle_event(event) &&
                event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                event.button.button == SDL_BUTTON_LEFT) {
                accepted = true;
                done = true;
                break;
            }
        }

        render_dialog_frame(modal.renderer, title, label, kDialogWidth, kTextDialogHeight);
        input.render(modal.renderer);
        cancel.render(modal.renderer);
        ok.render(modal.renderer);
        SDL_RenderPresent(modal.renderer);
    }

    input.stop_editing();
    raise_parent(parent);
    if (!accepted) {
        return std::nullopt;
    }
    return input.value();
}

std::vector<std::filesystem::path> open_files(SDL_Window* parent,
                                              const std::string& title,
                                              const std::filesystem::path& default_location,
                                              const std::vector<FileDialogFilter>& filters,
                                              bool allow_many) {
    return run_file_dialog(parent, SDL_FILEDIALOG_OPENFILE, title, default_location, filters, allow_many);
}

std::optional<std::filesystem::path> open_file(SDL_Window* parent,
                                               const std::string& title,
                                               const std::filesystem::path& default_location,
                                               const std::vector<FileDialogFilter>& filters) {
    auto paths = open_files(parent, title, default_location, filters, false);
    if (paths.empty()) {
        return std::nullopt;
    }
    return paths.front();
}

std::optional<std::filesystem::path> open_folder(SDL_Window* parent,
                                                 const std::string& title,
                                                 const std::filesystem::path& default_location) {
    auto paths = run_file_dialog(parent, SDL_FILEDIALOG_OPENFOLDER, title, default_location, {}, false);
    if (paths.empty()) {
        return std::nullopt;
    }
    return paths.front();
}

}  // namespace devmode::dialogs
