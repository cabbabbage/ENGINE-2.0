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
        std::uint32_t format = SDL_PIXELFORMAT_RGBA8888;
        TextureUploadOptions options{};
        std::vector<std::uint8_t> pixels{};

        bool valid() const { return width > 0 && height > 0 && pitch > 0 && !pixels.empty(); }
    };

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

    SDL_Texture* surface_to_texture(SDL_Renderer* renderer,
                                    SDL_Surface* surface,
                                    const TextureUploadOptions& options = TextureUploadOptions{});

    const PreparedGpuTextureUpload* prepared_gpu_upload_for_texture(SDL_Texture* texture);

    SDL_Texture* create_texture_from_prepared_upload(SDL_Renderer* renderer,
                                                     const PreparedGpuTextureUpload& prepared,
                                                     bool flip_horizontal = false,
                                                     bool flip_vertical = false);

    SDL_GPUTexture* upload_prepared_texture_to_gpu(SDL_GPUDevice* gpu_device,
                                                   const PreparedGpuTextureUpload& prepared,
                                                   std::string& out_error);

    bool save_bundle(const std::string& bundle_path, const BundleData& data);
    bool load_bundle(const std::string& bundle_path, BundleData& out_data);
    bool update_bundle_content_hash(const std::string& bundle_path, std::uint64_t content_hash);

}
