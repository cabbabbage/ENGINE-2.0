#ifndef DEV_MODE_UTILS_HPP
#define DEV_MODE_UTILS_HPP

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <unordered_map>

namespace devmode::utils {

inline SDL_Color with_alpha(SDL_Color c, Uint8 a) { c.a = a; return c; }

TTF_Font* load_font(int size);
std::string trim_whitespace_copy(const std::string& value);
std::string normalize_asset_name(std::string value);

}

#endif

