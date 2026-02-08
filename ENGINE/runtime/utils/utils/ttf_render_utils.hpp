#pragma once

#include <SDL3_ttf/SDL_ttf.h>

#include <string_view>

namespace ttf_util {

inline SDL_Surface* RenderTextBlended(TTF_Font* font, std::string_view text, SDL_Color color) {
    return TTF_RenderText_Blended(font, text.data(), text.size(), color);
}

inline SDL_Surface* RenderTextBlendedWrapped(TTF_Font* font, std::string_view text, SDL_Color color, int wrap_width) {
    return TTF_RenderText_Blended_Wrapped(font, text.data(), text.size(), color, wrap_width);
}

inline bool GetStringSize(TTF_Font* font, std::string_view text, int* w, int* h) {
    return TTF_GetStringSize(font, text.data(), text.size(), w, h);
}

}  // namespace ttf_util
