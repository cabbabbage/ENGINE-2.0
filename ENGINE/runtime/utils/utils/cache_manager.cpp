#include "cache_manager.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>
#include <cstring>

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

bool load_surface_sequence(const std::string& folder, int frame_count, std::vector<SDL_Surface*>& surfaces) {
    surfaces.clear();
    surfaces.reserve(frame_count);

    for (int i = 0; i < frame_count; ++i) {
        std::string frame_path = folder + "/" + std::to_string(i) + ".png";
        SDL_Surface* surface = IMG_Load(frame_path.c_str());
        if (!surface) {
            std::cerr << "[CacheManager] Failed to load surface from: " << frame_path
                      << " (IMG_Error: " << IMG_GetError() << ")" << std::endl;

            for (SDL_Surface* surf : surfaces) {
                if (surf) SDL_FreeSurface(surf);
            }
            surfaces.clear();
            return false;
        }
        surfaces.push_back(surface);
    }

    return true;
}

bool save_surface_sequence(const std::string& folder, const std::vector<SDL_Surface*>& surfaces) {
    std::cerr << "CacheManager::save_surface_sequence called - this should not happen in new architecture!" << std::endl;
    std::cerr << "Folder: " << folder << ", surfaces: " << surfaces.size() << std::endl;
    return false;
}

bool load_metadata(const std::string& file_path, nlohmann::json& metadata) {
    try {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return false;
        }
        file >> metadata;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load metadata from " << file_path << ": " << e.what() << std::endl;
        return false;
    }
}

bool save_metadata(const std::string& file_path, const nlohmann::json& metadata) {
    try {
        std::filesystem::create_directories(std::filesystem::path(file_path).parent_path());
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return false;
        }
        file << metadata.dump(2);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to save metadata to " << file_path << ": " << e.what() << std::endl;
        return false;
    }
}

SDL_Surface* load_surface(const std::string& file_path) {
    if (file_path.empty()) {
        return nullptr;
    }
    SDL_Surface* surface = IMG_Load(file_path.c_str());
    if (!surface) {
        std::cerr << "Failed to load surface from " << file_path << ": " << IMG_GetError() << std::endl;
    }
    return surface;
}

SDL_Texture* surface_to_texture(SDL_Renderer* renderer, SDL_Surface* surface) {
    if (!renderer || !surface) {
        return nullptr;
    }
    return SDL_CreateTextureFromSurface(renderer, surface);
}

std::optional<nlohmann::json> load_metadata(const std::string& meta_file) {
    nlohmann::json metadata;
    if (load_metadata(meta_file, metadata)) {
        return metadata;
    }
    return std::nullopt;
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
                if (!variant.foreground.empty()) {
                    const std::uint64_t fg_offset = payload.size();
                    payload.insert(payload.end(), variant.foreground.pixels.begin(), variant.foreground.pixels.end());
                    variant_node["foreground"] = describe_layer(variant.foreground, fg_offset);
                }
                if (!variant.background.empty()) {
                    const std::uint64_t bg_offset = payload.size();
                    payload.insert(payload.end(), variant.background.pixels.begin(), variant.background.pixels.end());
                    variant_node["background"] = describe_layer(variant.background, bg_offset);
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
                        if (variant_node.contains("foreground")) {
                            variant.foreground = read_layer(variant_node["foreground"], payload);
                        }
                        if (variant_node.contains("background")) {
                            variant.background = read_layer(variant_node["background"], payload);
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

}
