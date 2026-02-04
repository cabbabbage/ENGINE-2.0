#pragma once

#include <SDL.h>
#include <SDL_image.h>

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace CacheManager {

    struct BundleFrameLayer {
        int width = 0;
        int height = 0;
        std::uint32_t format = SDL_PIXELFORMAT_RGBA8888;
        int pitch = 0;
        std::vector<std::uint8_t> pixels;
        bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    };

    struct BundleFrameVariant {
        BundleFrameLayer base;
        BundleFrameLayer foreground;
        BundleFrameLayer background;
        bool use_atlas = false;
        SDL_Rect atlas_rect{0, 0, 0, 0};
    };

    struct BundleFrame {
        std::vector<BundleFrameVariant> variants;
    };

    struct BundleAnimation {
        std::string name;
        std::vector<float> variant_steps;
        std::vector<BundleFrame> frames;
        bool uses_atlas = false;
        std::vector<std::filesystem::path> atlas_paths;
    };

    struct BundleData {
        std::uint32_t version = 1;
        std::uint64_t content_hash = 0;
        nlohmann::json metadata_snapshot;
        std::vector<BundleAnimation> animations;
    };

    SDL_Surface* load_surface(const std::string& path);

    SDL_Texture* surface_to_texture(SDL_Renderer* renderer, SDL_Surface* surface);

    bool save_bundle(const std::string& bundle_path, const BundleData& data);
    bool load_bundle(const std::string& bundle_path, BundleData& out_data);

}
