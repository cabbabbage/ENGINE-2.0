#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace devmode::dialogs {

enum class MessageIcon {
    Info,
    Warning,
    Error,
};

struct DialogButton {
    int id = 0;
    std::string label;
    bool return_default = false;
    bool escape_default = false;
    bool destructive = false;
};

struct FileDialogFilter {
    std::string name;
    std::string pattern;
};

std::optional<int> show_choice(SDL_Window* parent,
                               const std::string& title,
                               const std::string& message,
                               const std::vector<DialogButton>& buttons,
                               MessageIcon icon = MessageIcon::Info);

bool confirm(SDL_Window* parent,
             const std::string& title,
             const std::string& message,
             const std::string& confirm_label = "OK",
             const std::string& cancel_label = "Cancel",
             bool destructive = false);

void show_message(SDL_Window* parent,
                  const std::string& title,
                  const std::string& message,
                  MessageIcon icon = MessageIcon::Info);

std::optional<std::string> prompt_text(SDL_Window* parent,
                                       const std::string& title,
                                       const std::string& label,
                                       const std::string& initial_value);

std::vector<std::filesystem::path> open_files(SDL_Window* parent,
                                              const std::string& title,
                                              const std::filesystem::path& default_location,
                                              const std::vector<FileDialogFilter>& filters,
                                              bool allow_many);

std::optional<std::filesystem::path> open_file(SDL_Window* parent,
                                               const std::string& title,
                                               const std::filesystem::path& default_location,
                                               const std::vector<FileDialogFilter>& filters);

std::optional<std::filesystem::path> open_folder(SDL_Window* parent,
                                                 const std::string& title,
                                                 const std::filesystem::path& default_location);

void raise_parent(SDL_Window* parent);

}  // namespace devmode::dialogs
