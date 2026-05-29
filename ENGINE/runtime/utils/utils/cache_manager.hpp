#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_image/SDL_image.h>

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace CacheManager {

    enum class TextureSemantic {
        Color,
        NormalMap
    };

    struct TextureUploadOptions {
        TextureSemantic semantic = TextureSemantic::Color;
        bool enable_mipmaps = true;
    };

    struct PreparedGpuTextureUpload {
        int width = 0;
        int height = 0;
        int pitch = 0;
        std::uint32_t format = SDL_PIXELFORMAT_RGBA32;
        TextureUploadOptions options{};
        std::vector<std::uint8_t> pixels{};

        bool valid() const { return width > 0 && height > 0 && pitch > 0 && !pixels.empty(); }
    };

    struct BundleFrameLayer {
        int width = 0;
        int height = 0;
        std::uint32_t format = SDL_PIXELFORMAT_RGBA32;
        int pitch = 0;
        std::vector<std::uint8_t> pixels;
        bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
    };

    // One texture layer per frame — no more multi-variant per frame.
    struct BundleFrame {
        BundleFrameLayer base_layer;
    };

    struct BundleAnimation {
        std::string name;
        std::vector<BundleFrame> frames;
    };

    struct BundleData {
        std::uint32_t version = 3;  // bumped from 2: removed BundleFrameTextureBinding, single layer per frame
        nlohmann::json metadata_snapshot;
        std::vector<BundleAnimation> animations;
    };

    SDL_Surface* load_surface(const std::string& path);

    SDL_Texture* surface_to_texture(SDL_Renderer* renderer,
                                    SDL_Surface* surface,
                                    const TextureUploadOptions& options = TextureUploadOptions{});

    const PreparedGpuTextureUpload* prepared_gpu_upload_for_texture(SDL_Texture* texture);
    void unregister_prepared_gpu_upload(SDL_Texture* texture);

    SDL_Texture* create_texture_from_prepared_upload(SDL_Renderer* renderer,
                                                     const PreparedGpuTextureUpload& prepared,
                                                     bool flip_horizontal = false,
                                                     bool flip_vertical = false);

    SDL_GPUTexture* upload_prepared_texture_to_gpu(SDL_GPUDevice* gpu_device,
                                                   const PreparedGpuTextureUpload& prepared,
                                                   std::string& out_error);

    bool save_bundle(const std::string& bundle_path, const BundleData& data);
    bool load_bundle(const std::string& bundle_path, BundleData& out_data);

}