#include "fog_renderer.hpp"
#include "utils/log.hpp"
#include <SDL_image.h>
#include <algorithm>
#include <cmath>

FogRenderer::FogRenderer(SDL_Renderer* renderer)
    : renderer_(renderer), loaded_(false) {
}

FogRenderer::~FogRenderer() {
    cleanup();
}

void FogRenderer::cleanup() {
    for (auto& fog_set : fog_textures_) {
        for (SDL_Texture* tex : fog_set.variants) {
            if (tex) {
                SDL_DestroyTexture(tex);
            }
        }
    }
    fog_textures_.clear();
    loaded_ = false;
}

bool FogRenderer::load_fog_textures(const std::string& fog_dir) {
    if (!renderer_) {
        vibble::log::error("[FogRenderer] Cannot load fog textures: renderer is null");
        return false;
    }

    cleanup();

    vibble::log::info("[FogRenderer] Loading fog textures from: " + fog_dir);

    fog_textures_.reserve(kNumFogTextures);
    int loaded_count = 0;

    for (int i = 1; i <= kNumFogTextures; ++i) {
        std::string filename = fog_dir + "/fog_" + std::to_string(i) + ".png";

        SDL_Surface* surface = IMG_Load(filename.c_str());
        if (!surface) {
            vibble::log::warn("[FogRenderer] Failed to load fog texture: " + filename + " - " + std::string(IMG_GetError()));
            continue;
        }

        SDL_Texture* base_texture = SDL_CreateTextureFromSurface(renderer_, surface);
        const int base_w = surface->w;
        const int base_h = surface->h;
        SDL_FreeSurface(surface);

        if (!base_texture) {
            vibble::log::error("[FogRenderer] Failed to create texture from surface: " + filename);
            continue;
        }

        FogVariantSet variant_set;
        variant_set.base_width = base_w;
        variant_set.base_height = base_h;

        if (!create_fog_variants(base_texture, variant_set)) {
            vibble::log::error("[FogRenderer] Failed to create variants for: " + filename);
            SDL_DestroyTexture(base_texture);
            continue;
        }

        SDL_DestroyTexture(base_texture);
        fog_textures_.push_back(std::move(variant_set));
        ++loaded_count;
    }

    loaded_ = (loaded_count > 0);

    if (loaded_) {
        vibble::log::info("[FogRenderer] Successfully loaded " + std::to_string(loaded_count) +
                         " fog textures with " + std::to_string(kNumFogVariants) + " variants each");
    } else {
        vibble::log::error("[FogRenderer] Failed to load any fog textures from: " + fog_dir);
    }

    return loaded_;
}

bool FogRenderer::create_fog_variants(SDL_Texture* base_texture, FogVariantSet& variant_set) {
    if (!base_texture || !renderer_) {
        return false;
    }

    int base_w = variant_set.base_width;
    int base_h = variant_set.base_height;

    if (base_w <= 0 || base_h <= 0) {
        SDL_QueryTexture(base_texture, nullptr, nullptr, &base_w, &base_h);
        variant_set.base_width = base_w;
        variant_set.base_height = base_h;
    }

    // Create 10 variants: 10%, 20%, ..., 100%
    for (int i = 0; i < kNumFogVariants; ++i) {
        const float scale_factor = (i + 1) * 0.1f;  // 0.1, 0.2, ..., 1.0
        const int variant_w = std::max(1, static_cast<int>(std::lround(base_w * scale_factor)));
        const int variant_h = std::max(1, static_cast<int>(std::lround(base_h * scale_factor)));

        // Create render target texture
        SDL_Texture* variant_texture = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET,
            variant_w,
            variant_h
        );

        if (!variant_texture) {
            vibble::log::error("[FogRenderer] Failed to create variant texture at scale " +
                             std::to_string(scale_factor));
            // Clean up previously created variants
            for (int j = 0; j < i; ++j) {
                if (variant_set.variants[j]) {
                    SDL_DestroyTexture(variant_set.variants[j]);
                    variant_set.variants[j] = nullptr;
                }
            }
            return false;
        }

        // Set blend mode for the variant texture
        SDL_SetTextureBlendMode(variant_texture, SDL_BLENDMODE_BLEND);

        // Render the base texture scaled to the variant texture
        SDL_Texture* old_target = SDL_GetRenderTarget(renderer_);
        SDL_SetRenderTarget(renderer_, variant_texture);

        // Clear with transparent background
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);

        // Render scaled base texture
        SDL_Rect dest_rect = {0, 0, variant_w, variant_h};
        SDL_RenderCopy(renderer_, base_texture, nullptr, &dest_rect);

        // Restore previous render target
        SDL_SetRenderTarget(renderer_, old_target);

        variant_set.variants[i] = variant_texture;
    }

    return true;
}

SDL_Texture* FogRenderer::get_fog_variant(int fog_index, int target_size) const {
    if (!loaded_ || fog_index < 0 || fog_index >= static_cast<int>(fog_textures_.size())) {
        return nullptr;
    }

    const FogVariantSet& fog_set = fog_textures_[fog_index];

    // Determine which variant to use based on target size
    // Calculate what percentage of base size the target is
    const int max_base_dim = std::max(fog_set.base_width, fog_set.base_height);
    if (max_base_dim <= 0) {
        return fog_set.variants[kNumFogVariants - 1];  // Return largest variant
    }

    const float size_ratio = static_cast<float>(target_size) / static_cast<float>(max_base_dim);

    // Map to variant index (0-9 for 10%-100%)
    int variant_index = std::max(0, static_cast<int>(std::ceil(size_ratio * 10.0f)) - 1);
    variant_index = std::min(variant_index, kNumFogVariants - 1);

    return fog_set.variants[variant_index];
}

float FogRenderer::calculate_fog_opacity(float distance_from_camera) {
    if (distance_from_camera <= kFogForegroundDistance) {
        return 0.0f;  // No fog
    }
    if (distance_from_camera >= kFogBackgroundDistance) {
        return 1.0f;  // Full fog
    }

    // Linear interpolation between foreground and background distances
    const float range = kFogBackgroundDistance - kFogForegroundDistance;
    const float normalized = (distance_from_camera - kFogForegroundDistance) / range;

    return std::clamp(normalized, 0.0f, 1.0f);
}
