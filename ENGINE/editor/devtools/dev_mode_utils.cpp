#include "dev_mode_utils.hpp"

#include "dm_styles.hpp"
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace devmode::utils {

TTF_Font* load_font(int size) {
    static std::unordered_map<int, TTF_Font*> cache;
    auto it = cache.find(size);
    if (it != cache.end()) return it->second;

    const DMLabelStyle& label = DMStyles::Label();
    TTF_Font* font = TTF_OpenFont(label.font_path.c_str(), size);
    if (!font) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[DevModeUtils] Failed to load font '%s' size %d: %s", label.font_path.c_str(), size, SDL_GetError());
        return nullptr;
    }
    cache.emplace(size, font);
    return font;
}

std::string trim_whitespace_copy(const std::string& value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    while (end != begin) {
        auto prev = end; --prev;
        if (!std::isspace(static_cast<unsigned char>(*prev))) break;
        end = prev;
    }
    return std::string(begin, end);
}

std::string normalize_asset_name(std::string value) {
    std::string trimmed = trim_whitespace_copy(value);
    std::string result;
    result.reserve(trimmed.size());
    bool last_was_separator = false;
    for (char ch : trimmed) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            result.push_back(static_cast<char>(std::tolower(uch)));
            last_was_separator = false;
        } else if (ch == '_' || ch == '-' || std::isspace(uch)) {
            if (!result.empty() && !last_was_separator) {
                result.push_back('_');
                last_was_separator = true;
            }
        }
    }
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }
    return result;
}

}
