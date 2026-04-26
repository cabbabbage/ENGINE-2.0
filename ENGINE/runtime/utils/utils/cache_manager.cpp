#include "cache_manager.hpp"
#include <nlohmann/json.hpp>
#include <SDL3/SDL_gpu.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>
#include <cstring>
#include <limits>

#include "utils/log.hpp"

namespace CacheManager {

namespace {

constexpr std::uint32_t kBundleVersion = 1;
constexpr std::uint64_t kBundleMagic = 0x424e444c454e4745ull; // "ENGENDLB" little endian folded

#pragma pack(push, 1)
struct BundleHeader {
    std::uint64_t magic = kBundleMagic;
    std::uint32_t version = kBundleVersion;
    std::uint32_t reserved = 0;
    std::uint64_t metadata_size = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t content_hash = 0;
};
#pragma pack(pop)

std::uint64_t fnv1a(const void* data, std::size_t len, std::uint64_t seed) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr Uint32 kD3D12UploadRowAlignment = 256u;

std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const std::size_t remainder = value % alignment;
    return remainder == 0 ? value : (value + alignment - remainder);
}

bool is_mipmap_beneficial(const SDL_Surface* surface, const CacheManager::TextureUploadOptions& options) {
    if (!surface || !options.enable_mipmaps) {
        return false;
    }
    return surface->w >= 128 && surface->h >= 128;
}

void log_texture_policy_once(SDL_GPUDevice* gpu_device,
                             const CacheManager::TextureUploadOptions& options) {
    static bool logged_color = false;
    static bool logged_normals = false;
    bool& logged = (options.semantic == CacheManager::TextureSemantic::NormalMap) ? logged_normals : logged_color;
    if (logged || !gpu_device) {
        return;
    }
    logged = true;

    const bool supports_bc7 = SDL_GPUTextureSupportsFormat(gpu_device,
                                                            SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM,
                                                            SDL_GPU_TEXTURETYPE_2D,
                                                            SDL_GPU_TEXTUREUSAGE_SAMPLER);
    const bool supports_bc5 = SDL_GPUTextureSupportsFormat(gpu_device,
                                                            SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM,
                                                            SDL_GPU_TEXTURETYPE_2D,
                                                            SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (options.semantic == CacheManager::TextureSemantic::NormalMap) {
        vibble::log::info("[CacheManager] Texture upload policy(normal): preferred=BC5 supported=" +
                          std::string(supports_bc5 ? "1" : "0") +
                          " upload=RGBA8 (source assets are uncompressed)");
    } else {
        vibble::log::info("[CacheManager] Texture upload policy(color): preferred=BC7 supported=" +
                          std::string(supports_bc7 ? "1" : "0") +
                          " upload=RGBA8 (source assets are uncompressed)");
    }
}

bool upload_surface_via_transfer_buffer(SDL_Renderer* renderer,
                                        SDL_Texture* texture,
                                        SDL_Surface* surface,
                                        const CacheManager::TextureUploadOptions& options) {
    if (!renderer || !texture || !surface) {
        return false;
    }

    SDL_PropertiesID renderer_props = SDL_GetRendererProperties(renderer);
    if (!renderer_props) {
        return false;
    }
    SDL_GPUDevice* gpu_device = static_cast<SDL_GPUDevice*>(
        SDL_GetPointerProperty(renderer_props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
    if (!gpu_device) {
        return false;
    }

    SDL_PropertiesID texture_props = SDL_GetTextureProperties(texture);
    if (!texture_props) {
        return false;
    }
    SDL_GPUTexture* gpu_texture = static_cast<SDL_GPUTexture*>(
        SDL_GetPointerProperty(texture_props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    if (!gpu_texture) {
        return false;
    }

    log_texture_policy_once(gpu_device, options);

    if (surface->w <= 0 || surface->h <= 0 || surface->pitch <= 0) {
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(surface->w) * 4u;
    const std::size_t upload_row_bytes = align_up(row_bytes, kD3D12UploadRowAlignment);
    const std::size_t upload_bytes = upload_row_bytes * static_cast<std::size_t>(surface->h);
    if (upload_bytes == 0 || upload_bytes > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        return false;
    }
    if (upload_row_bytes > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transfer_create{};
    transfer_create.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_create.size = static_cast<Uint32>(upload_bytes);
    transfer_create.props = 0;
    SDL_GPUTransferBuffer* transfer_buffer = SDL_CreateGPUTransferBuffer(gpu_device, &transfer_create);
    if (!transfer_buffer) {
        return false;
    }

    bool uploaded = false;
    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, transfer_buffer, true);
    if (mapped) {
        std::uint8_t* dst = static_cast<std::uint8_t*>(mapped);
        const std::uint8_t* src = static_cast<const std::uint8_t*>(surface->pixels);
        for (int row = 0; row < surface->h; ++row) {
            std::memcpy(dst + static_cast<std::size_t>(row) * upload_row_bytes,
                        src + static_cast<std::size_t>(row) * static_cast<std::size_t>(surface->pitch),
                        row_bytes);
        }
        SDL_UnmapGPUTransferBuffer(gpu_device, transfer_buffer);

        SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(gpu_device);
        if (command_buffer) {
            SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
            if (copy_pass) {
                SDL_GPUTextureTransferInfo source{};
                source.transfer_buffer = transfer_buffer;
                source.offset = 0;
                source.pixels_per_row = static_cast<Uint32>(upload_row_bytes / 4u);
                source.rows_per_layer = static_cast<Uint32>(surface->h);

                SDL_GPUTextureRegion destination{};
                destination.texture = gpu_texture;
                destination.mip_level = 0;
                destination.layer = 0;
                destination.x = 0;
                destination.y = 0;
                destination.z = 0;
                destination.w = static_cast<Uint32>(surface->w);
                destination.h = static_cast<Uint32>(surface->h);
                destination.d = 1;

                SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
                SDL_EndGPUCopyPass(copy_pass);

                if (is_mipmap_beneficial(surface, options)) {
                    SDL_GenerateMipmapsForGPUTexture(command_buffer, gpu_texture);
                }

                uploaded = SDL_SubmitGPUCommandBuffer(command_buffer);
                if (!uploaded) {
                    SDL_CancelGPUCommandBuffer(command_buffer);
                }
            } else {
                SDL_CancelGPUCommandBuffer(command_buffer);
            }
        }
    }

    SDL_ReleaseGPUTransferBuffer(gpu_device, transfer_buffer);
    return uploaded;
}

nlohmann::json describe_layer(const BundleFrameLayer& layer, std::uint64_t offset) {
    nlohmann::json node = nlohmann::json::object();
    node["offset"] = offset;
    node["size"] = static_cast<std::uint64_t>(layer.pixels.size());
    node["width"] = layer.width;
    node["height"] = layer.height;
    node["pitch"] = layer.pitch;
    node["format"] = layer.format;
    return node;
}

BundleFrameLayer read_layer(const nlohmann::json& node,
                            const std::vector<std::uint8_t>& payload) {
    BundleFrameLayer layer;
    if (!node.is_object()) {
        return layer;
    }
    const std::uint64_t offset = node.value("offset", static_cast<std::uint64_t>(0));
    const std::uint64_t size = node.value("size", static_cast<std::uint64_t>(0));
    if (size == 0 || offset + size > payload.size()) {
        return layer;
    }
    layer.width = node.value("width", 0);
    layer.height = node.value("height", 0);
    layer.pitch = node.value("pitch", 0);
    layer.format = node.value("format", SDL_PIXELFORMAT_RGBA8888);
    layer.pixels.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                        payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
    return layer;
}

} // namespace


SDL_Surface* load_surface(const std::string& file_path) {
    if (file_path.empty()) {
        return nullptr;
    }
    SDL_Surface* surface = IMG_Load(file_path.c_str());
    if (!surface) {
        std::cerr << "Failed to load surface from " << file_path << ": " << SDL_GetError() << std::endl;
    }
    return surface;
}

SDL_Texture* surface_to_texture(SDL_Renderer* renderer,
                                SDL_Surface* surface,
                                const TextureUploadOptions& options) {
    if (!renderer || !surface) {
        return nullptr;
    }
    SDL_Surface* rgba_surface = surface;
    bool owns_rgba_surface = false;
    if (surface->format != SDL_PIXELFORMAT_RGBA8888) {
        rgba_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
        if (!rgba_surface) {
            return SDL_CreateTextureFromSurface(renderer, surface);
        }
        owns_rgba_surface = true;
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_STATIC,
                                             rgba_surface->w,
                                             rgba_surface->h);
    if (texture) {
        bool uploaded = upload_surface_via_transfer_buffer(renderer,
                                                           texture,
                                                           rgba_surface,
                                                           options);
        if (!uploaded) {
            uploaded = SDL_UpdateTexture(texture,
                                         nullptr,
                                         rgba_surface->pixels,
                                         rgba_surface->pitch);
        }
        if (!uploaded) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
    }

    if (!texture) {
        texture = SDL_CreateTextureFromSurface(renderer, rgba_surface);
    }

    if (owns_rgba_surface) {
        SDL_DestroySurface(rgba_surface);
    }
    return texture;
}

bool save_bundle(const std::string& bundle_path, const BundleData& data) {
    try {
        std::filesystem::create_directories(std::filesystem::path(bundle_path).parent_path());
    } catch (...) {
        std::cerr << "[CacheManager] Failed to create bundle directory for " << bundle_path << "\n";
        return false;
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(1024 * 1024); // reserve 1MB to cut down reallocations

    nlohmann::json meta = nlohmann::json::object();
    meta["version"] = data.version;
    meta["hash"] = data.content_hash;
    meta["metadata_snapshot"] = data.metadata_snapshot;

    nlohmann::json anims = nlohmann::json::array();
    for (const auto& anim : data.animations) {
        nlohmann::json anim_node;
        anim_node["name"] = anim.name;
        anim_node["variant_steps"] = anim.variant_steps;
        anim_node["uses_atlas"] = anim.uses_atlas;
        nlohmann::json atlas_paths_json = nlohmann::json::array();
        for (const auto& p : anim.atlas_paths) {
            atlas_paths_json.push_back(p.generic_string());
        }
        anim_node["atlas_paths"] = std::move(atlas_paths_json);

        nlohmann::json frames = nlohmann::json::array();
        for (const auto& frame : anim.frames) {
            nlohmann::json frame_node = nlohmann::json::array();
            for (const auto& variant : frame.variants) {
                nlohmann::json variant_node;
                const std::uint64_t base_offset = payload.size();
                if (!variant.base.empty()) {
                    payload.insert(payload.end(), variant.base.pixels.begin(), variant.base.pixels.end());
                    variant_node["base"] = describe_layer(variant.base, base_offset);
                }

                if (variant.use_atlas) {
                    variant_node["atlas_rect"] = {
                        {"x", variant.atlas_rect.x},
                        {"y", variant.atlas_rect.y},
                        {"w", variant.atlas_rect.w},
                        {"h", variant.atlas_rect.h}
                    };
                    variant_node["use_atlas"] = true;
                } else {
                    variant_node["use_atlas"] = false;
                }
                frame_node.push_back(std::move(variant_node));
            }
            frames.push_back(std::move(frame_node));
        }
        anim_node["frames"] = std::move(frames);
        anims.push_back(std::move(anim_node));
    }
    meta["animations"] = std::move(anims);

    const std::string meta_text = meta.dump();

    BundleHeader header{};
    header.magic = kBundleMagic;
    header.version = kBundleVersion;
    header.metadata_size = meta_text.size();
    header.payload_size = payload.size();
    header.content_hash = data.content_hash;

    std::ofstream out(bundle_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[CacheManager] Failed to open bundle for writing: " << bundle_path << "\n";
        return false;
    }

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(meta_text.data(), static_cast<std::streamsize>(meta_text.size()));
    if (!payload.empty()) {
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    out.flush();
    return out.good();
}

bool load_bundle(const std::string& bundle_path, BundleData& out_data) {
    std::ifstream in(bundle_path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    BundleHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good()) {
        return false;
    }
    if (header.magic != kBundleMagic || header.version != kBundleVersion) {
        std::cerr << "[CacheManager] Bundle magic/version mismatch for " << bundle_path << "\n";
        return false;
    }

    std::string meta_text;
    meta_text.resize(static_cast<std::size_t>(header.metadata_size));
    in.read(meta_text.data(), static_cast<std::streamsize>(meta_text.size()));
    if (!in.good()) {
        return false;
    }
    nlohmann::json meta;
    try {
        meta = nlohmann::json::parse(meta_text);
    } catch (const std::exception& e) {
        std::cerr << "[CacheManager] Failed to parse bundle metadata: " << e.what() << "\n";
        return false;
    }

    std::vector<std::uint8_t> payload;
    if (header.payload_size > 0) {
        payload.resize(static_cast<std::size_t>(header.payload_size));
        in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!in.good()) {
            return false;
        }
    }

    out_data = BundleData{};
    out_data.version = header.version;
    out_data.content_hash = header.content_hash;
    out_data.metadata_snapshot = meta.value("metadata_snapshot", nlohmann::json::object());

    if (meta.contains("animations") && meta["animations"].is_array()) {
        for (const auto& anim_node : meta["animations"]) {
            if (!anim_node.is_object()) continue;
            BundleAnimation anim;
            anim.name = anim_node.value("name", std::string{});
            anim.variant_steps = anim_node.value("variant_steps", std::vector<float>{});
            anim.uses_atlas = anim_node.value("uses_atlas", false);
            if (anim_node.contains("atlas_paths") && anim_node["atlas_paths"].is_array()) {
                for (const auto& entry : anim_node["atlas_paths"]) {
                    if (entry.is_string()) {
                        anim.atlas_paths.push_back(entry.get<std::string>());
                    }
                }
            }

            if (anim_node.contains("frames") && anim_node["frames"].is_array()) {
                for (const auto& frame_node : anim_node["frames"]) {
                    BundleFrame frame;
                    if (!frame_node.is_array()) {
                        anim.frames.push_back(std::move(frame));
                        continue;
                    }
                    for (const auto& variant_node : frame_node) {
                        BundleFrameVariant variant;
                        if (variant_node.contains("base")) {
                            variant.base = read_layer(variant_node["base"], payload);
                        }
                        variant.use_atlas = variant_node.value("use_atlas", false);
                        if (variant_node.contains("atlas_rect") && variant_node["atlas_rect"].is_object()) {
                            const auto& rect = variant_node["atlas_rect"];
                            variant.atlas_rect.x = rect.value("x", 0);
                            variant.atlas_rect.y = rect.value("y", 0);
                            variant.atlas_rect.w = rect.value("w", 0);
                            variant.atlas_rect.h = rect.value("h", 0);
                        }
                        frame.variants.push_back(std::move(variant));
                    }
                    anim.frames.push_back(std::move(frame));
                }
            }

            out_data.animations.push_back(std::move(anim));
        }
    }

    return true;
}

bool update_bundle_content_hash(const std::string& bundle_path, std::uint64_t content_hash) {
    std::fstream io(bundle_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!io.is_open()) {
        return false;
    }

    BundleHeader header{};
    io.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!io.good()) {
        return false;
    }
    if (header.magic != kBundleMagic || header.version != kBundleVersion) {
        return false;
    }
    if (header.content_hash == content_hash) {
        return true;
    }

    header.content_hash = content_hash;
    io.clear();
    io.seekp(0, std::ios::beg);
    io.write(reinterpret_cast<const char*>(&header), sizeof(header));
    io.flush();
    return io.good();
}

}
